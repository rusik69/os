#include "slab.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "spinlock.h"
#include "printf.h"
#include "smp.h"
#include "io.h"
#include "rng.h"
#include "kasan_light.h"
#include "kmemleak.h"
#include "page_allocator_ext.h"

/*
 * ────────────────────────────────────────────────────────────────────────────
 * Slab Allocator — Fixed-Size Object Cache
 * ────────────────────────────────────────────────────────────────────────────
 *
 * OVERVIEW
 * --------
 * The slab allocator provides O(1) allocation and deallocation for fixed-size
 * kernel objects (e.g. process structures, inodes, dentries).  It reduces
 * internal fragmentation by packing same-sized objects into contiguous
 * physical pages (slabs), and improves cache locality by keeping objects of
 * the same type on contiguous memory.
 *
 * Each cache is a collection of slabs (contiguous physical pages, typically
 * 1-8 pages per slab). Objects are carved from slabs and tracked via an
 * intrusive free list stored directly in freed objects — no separate bitmap
 * overhead.  Slabs move between three doubly-linked lists as their occupancy
 * changes.
 *
 * SLAB STATE LISTS
 * ────────────────
 *   slabs_partial  — Some objects free, some allocated.  Preferred target
 *                    for allocation (no need to allocate a new slab).  A
 *                    single allocation here can fill the last free slot,
 *                    moving the slab to slabs_full.
 *
 *   slabs_full     — All objects are allocated.  No free objects available.
 *                    When an object is freed back, the slab becomes partial
 *                    (unless the free leaves all objects free, in which
 *                    case it goes to slabs_free).
 *
 *   slabs_free     — All objects are free (empty slab).  Candidate for
 *                    reaping (freeing pages back to PMM).  The reaper
 *                    periodically scans slabs_free lists and frees pages
 *                    to reclaim memory.
 *
 * STRUCTURE
 * ─────────
 *   struct kmem_cache  (line ~71) — Per-type cache descriptor:
 *     name          — Human-readable name (e.g. "task_struct")
 *     obj_size      — Actual object size (rounded+aligned, includes redzone)
 *     user_size     — Caller-requested size (without redzone)
 *     align         — Requested alignment
 *     gfporder      — 2^gfporder pages per slab
 *     num           — Objects per slab
 *     ctor          — Constructor for fresh objects (may be NULL)
 *     colour_off    — Max colour offset for cache-line coloring
 *     colour_next   — Next colour to use (cycling per slab)
 *     slabs_full    — Doubly-linked list of full slabs
 *     slabs_partial — Doubly-linked list of partial slabs
 *     slabs_free    — Doubly-linked list of free slabs
 *     cpu_slab[]    — Per-CPU object cache for lockless fast path
 *     next          — Linked list of all caches (for slab reaper)
 *     lock          — Spinlock protecting slab lists
 *
 *   struct slab  (line ~60) — Per-slab header at the start of each slab:
 *     next, prev   — Doubly-linked list pointers
 *     free_list    — Head of intrusive free object linked list
 *     free_count   — Number of free objects in this slab
 *     total        — Total objects in this slab
 *     state        — FULL / PARTIAL / FREE (which list this slab is in)
 *
 *   struct cpu_slab  (line ~47) — Per-CPU object cache:
 *     objects[]    — Array of pre-fetched free object pointers
 *     count        — Number of valid entries in objects[]
 *
 * PER-CPU FAST PATH
 * ─────────────────
 * Each cache maintains a small per-CPU array (SLAB_CPU_CACHE_SIZE = 8) of
 * pre-fetched free object pointers.  The fast allocation path (with IRQ
 * save/restore for reentrancy) pops from this array.  If empty, it
 * refills from the slab list under the cache's spinlock.  The fast free
 * path pushes to the per-CPU array.  If full, it flushes to the slab list
 * under the spinlock.
 *
 * This design provides a lockless fast path for the common case (no
 * atomic ops or spinlock contention) on SMP systems.  Only local IRQ
 * save/restore is needed for reentrancy from interrupt handlers on the
 * same CPU.
 *
 * ALLOCATION FLOW  (kmem_cache_alloc)
 * ─────────────────
 *   1. Try per-CPU cache: pop objects[--count] (fast, lockless).
 *   2. If per-CPU cache empty, acquire cache lock and try slabs_partial.
 *      - Iterate slabs_partial list, find a slab with free objects.
 *      - Pop from slab's free_list (intrusive linked list).
 *      - If slab becomes full, relink to slabs_full.
 *      - Refill per-CPU cache from remaining free objects.
 *   3. If no partial slab, try slabs_free.
 *      - Pop from slab's free_list.
 *      - Relink slab to slabs_partial.
 *   4. If no free slab, allocate new pages from PMM and create a new slab.
 *   5. On success: check free poison (UAF detection), apply alloc poison,
 *      set redzone, call constructor if present, call KASAN/kmemleak hooks.
 *   6. Return object pointer.
 *
 * FREE FLOW  (kmem_cache_free)
 * ──────────
 *   1. Check alloc poison (detect use-after-free by caller), check redzone
 *      (detect buffer overrun), apply free poison.
 *   2. Try per-CPU cache: push to objects[count++] (fast, lockless).
 *   3. If per-CPU cache full, acquire cache lock and flush per-CPU entries
 *      to the source slab's free list.
 *      - Update slab's free_count.
 *      - Relink slab between slabs_partial, slabs_full, slabs_free as needed.
 *   4. Call KASAN/kmemleak hooks.
 *
 * POISONING & REDZONE
 * ───────────────────
 *   SLAB_POISON_FREE  (0x6b) — Fills freed objects to detect UAF writes.
 *     The first 8 bytes are left intact (used for the free-list pointer).
 *     On alloc, bytes 8-15 are checked — if disturbed, a UAF write occurred.
 *
 *   SLAB_POISON_ALLOC (0x6a) — Fills freshly allocated objects to detect
 *     use of uninitialized data.
 *
 *   SLAB_REDZONE_SIZE (8 bytes at end of each object) — Filled with
 *     0xFDFDFDFDFDFDFDFD on alloc.  Checked on free.  If disturbed, the
 *     caller wrote past the end of the object (buffer overflow).
 *
 * RANDOM FREELIST INSERTION
 * ──────────────────────────
 * Instead of always pushing freed objects to the head of the slab's free
 * list (LIFO), freed objects are inserted at a random depth of up to
 * FREELIST_RANDOM_DEPTH (4) entries.  This scrambles the allocation order
 * so that sequential kmem_cache_alloc calls return unpredictably-arranged
 * addresses, making heap exploits harder to construct.
 *
 * CACHE-LINE COLOURING
 * ────────────────────
 * Slabs within the same cache are shifted by a different colour offset
 * (colour_off, cycling through colour_next).  This ensures that objects
 * at the same relative offset in different slabs map to different cache
 * lines, reducing cache-line contention on SMP systems.
 *
 * SLAB REAPER
 * ───────────
 * A periodic slab_reap() function scans all caches (via the cache_list
 * linked list) and frees pages from slabs_free lists back to PMM.  This
 * reclaims memory from caches whose objects have all been freed.
 * ────────────────────────────────────────────────────────────────────────────
 */

/* Size of per-CPU object cache (number of free object pointers per CPU) */
#define SLAB_CPU_CACHE_SIZE 8

/* Object poisoning and redzoning — detect use-after-free and buffer overruns */
#define SLAB_POISON_FREE  0x6b  /* fill freed objects with this pattern */
#define SLAB_POISON_ALLOC 0x6a  /* fill freshly allocated objects with this */
#define SLAB_REDZONE_SIZE 8     /* redzone bytes at end of each object */
#define SLAB_REDZONE_PATTERN 0xFDULL /* fill redzone with this canary */

/* Helpers */
static inline uint64_t make_redzone_pattern(void) {
    uint64_t pat = 0;
    for (int i = 0; i < 8; i++)
        pat = (pat << 8) | SLAB_REDZONE_PATTERN;
    return pat;
}

/**
 * struct cpu_slab - Per-CPU object cache for the lockless fast path.
 * @objects: Array of cached free-object pointers (holds up to @SLAB_CPU_CACHE_SIZE).
 * @count: Number of valid entries currently in @objects.
 *
 * Each kmem_cache keeps one of these per CPU so that allocations and frees
 * on a hot path can be served from a per-CPU stack without taking the global
 * cache @kmem_cache.lock.  When the cache underflows or overflows, the slow
 * path refills or drains it under the cache lock.
 */
struct cpu_slab {
    void *objects[SLAB_CPU_CACHE_SIZE]; /* cached free object pointers */
    int   count;                         /* number of valid entries */
};

enum slab_state {
    SLAB_FULL    = 0,
    SLAB_PARTIAL = 1,
    SLAB_FREE    = 2,
};

/* ── Slab header (at the start of each slab's first page) ────────────── */

/**
 * struct slab - Slab header stored at the start of each slab's first page.
 * @next: Next slab in the cache list the slab is linked into.
 * @prev: Previous slab in the cache list.
 * @free_list: Head of the linked list of free objects within this slab.
 * @free_count: Number of free objects currently in this slab.
 * @total: Total number of objects this slab can hold.
 * @state: Which cache list (SLAB_FULL/PARTIAL/FREE) this slab is linked into.
 *
 * A slab is a run of physically-contiguous pages from which fixed-size
 * objects are carved.  Because allocations may span a non-power-of-two
 * object size the slab base address is always PAGE_SIZE-aligned, which is
 * the assumption relied on by the object-to-slab reverse mapping formula
 * `(struct slab *)((uint64_t)obj & ~(slab_size - 1))`.
 */
struct slab {
    struct slab    *next;          /* linked list in cache */
    struct slab    *prev;
    void           *free_list;     /* head of free object linked list */
    int             free_count;    /* number of free objects in this slab */
    int             total;         /* total objects in this slab */
    enum slab_state state;         /* which list this slab is linked into */
};

/**
 * struct kmem_cache - Per-type cache descriptor.
 * @name: Human-readable cache name (used in diagnostics).
 * @obj_size: Actual object size (rounded + aligned), includes the redzone.
 * @user_size: Caller-requested object size (without the redzone).
 * @align: Requested object alignment (power of two, minimum 8).
 * @gfporder: Order of the physically-contiguous pages backing each slab (2^gfporder pages).
 * @num: Number of objects per slab.
 * @colour_off: Maximum colour offset (leftover bytes) for cache-line coloring.
 * @colour_next: Next colour to use (cycles per slab).
 * @ctor: Constructor invoked on freshly allocated slab objects (may be NULL).
 * @slabs_full: List of slabs with no free objects.
 * @slabs_partial: List of slabs with some free objects.
 * @slabs_free: List of slabs whose objects are all free.
 * @lock: Spinlock serialising slab growth, drain, and teardown.
 * @cpu_slab: Per-CPU object caches for the lockless fast path.
 * @next: Next cache in the global cache_list (for the reaper and stats).
 */
struct kmem_cache {
    const char       *name;
    size_t            obj_size;   /* actual object size (rounded + aligned), includes redzone */
    size_t            user_size;  /* caller-requested object size (without redzone) */
    size_t            align;      /* requested alignment */
    int               gfporder;   /* 2^gfporder pages per slab */
    int               num;        /* objects per slab */
    int               colour_off; /* max colour offset (leftover bytes) for cache-line coloring */
    int               colour_next;/* next colour to use (cycling per slab) */
    kmem_cache_ctor_t ctor;       /* constructor for fresh objects (may be NULL) */

    struct slab      *slabs_full;
    struct slab      *slabs_partial;
    struct slab      *slabs_free;

    spinlock_t        lock;

    /* Per-CPU object cache for lockless fast path */
    struct cpu_slab   cpu_slab[SMP_MAX_CPUS];

    struct kmem_cache *next;     /* linked list of all caches (for reaper) */
};

/* ── Poisoning and redzone helpers ────────────────────────────────────── */

/* Verify free poison is intact (UAF detection on alloc).
 * Prints a warning if the object was written to while on the free list.
 * Called right before slab_poison_alloc in kmem_cache_alloc. */
static inline void slab_check_poison_free(struct kmem_cache *cache, void *obj) {
    /* Check bytes 8-15 (the first 8 bytes after the free-list pointer).
     * If disturbed (no longer SLAB_POISON_FREE), a UAF write likely
     * occurred while the object was on the free list. */
    uint64_t check = *(const uint64_t *)((const uint8_t *)obj + 8);
    uint64_t expected = 0;
    for (int i = 0; i < 8; i++)
        expected = (expected << 8) | SLAB_POISON_FREE;
    if (check != expected) {
        kprintf("[SLAB] UAF DETECTED in '%s': obj=%p, bytes 8-15 corrupted "
                "(expected 0x%016llx, actual 0x%016llx) — possible use-after-free write\n",
                cache->name, obj,
                (unsigned long long)expected,
                (unsigned long long)check);
    }
}

/* Write the redzone canary at the end of a freshly allocated object */
static inline void slab_set_redzone(struct kmem_cache *cache, void *obj) {
    uint64_t *rz = (uint64_t *)((uint8_t *)obj + cache->user_size);
    *rz = make_redzone_pattern();
}

/* Verify the redzone canary is intact.  Returns 0 on corruption. */
static inline int slab_check_redzone(struct kmem_cache *cache, void *obj) {
    uint64_t *rz = (uint64_t *)((uint8_t *)obj + cache->user_size);
    uint64_t expected = make_redzone_pattern();
    if (*rz != expected) {
        kprintf("[SLAB] REDZONE CORRUPTED in '%s': obj=%p, expected=0x%llx, actual=0x%llx\n",
                cache->name, obj, (unsigned long long)expected, (unsigned long long)*rz);
        return 0;
    }
    return 1;
}

/* ── Random freelist insertion ───────────────────────────────────────────
 *
 * Instead of always pushing freed objects to the head of the slab's free
 * list (LIFO), insert at a random depth of up to FREELIST_RANDOM_DEPTH
 * entries.  This scrambles the allocation order so that sequential kmalloc
 * calls return unpredictably-arranged addresses, making heap exploits
 * harder to construct.
 *
 * The depth is bounded to keep the operation O(1) in practice (the linked
 * list walk is at most a few pointer chases).  Objects freed to the head
 * of an empty list are placed at depth 0 (the only option).
 */
#define FREELIST_RANDOM_DEPTH 4

static void slab_freelist_insert_random(struct slab *slab, void *obj) {
    void **insert = &slab->free_list;
    /* Pick a random depth in [0, FREELIST_RANDOM_DEPTH).  Walk that far
     * into the list (or until we hit the end).  Then splice @obj in. */
    int depth = (int)(rng_get_u32() % (uint32_t)FREELIST_RANDOM_DEPTH);
    for (int i = 0; i < depth && *insert; i++)
        insert = (void **)(*insert);
    *(void **)obj = *insert;
    *insert = obj;
}

/* Poison a freshly freed object (before adding to free list).
 * The first 8 bytes are left intact for the free-list pointer. */
static inline void slab_poison_free(struct kmem_cache *cache, void *obj) {
    size_t poison_len = cache->obj_size;
    if (poison_len > 8) {
        memset((uint8_t *)obj + 8, SLAB_POISON_FREE, poison_len - 8);
    }
}

/* Poison a freshly allocated object before handing it to the caller. */
static inline void slab_poison_alloc(struct kmem_cache *cache, void *obj) {
    memset(obj, SLAB_POISON_ALLOC, cache->obj_size);
}

/* ── All-caches linked list ──────────────────────────────────────────── */

static struct kmem_cache *cache_list = NULL;
static int slab_initialized = 0;
static spinlock_t cache_list_lock = SPINLOCK_INIT;

/* ── Slab statistics ────────────────────────────────────────────────── */

/* ── Helper: free a slab's physically-contiguous pages to PMM ────────── */
static void slab_free_pages(struct slab *slab, int pages) {
    uint64_t phys = VIRT_TO_PHYS((uint64_t)(uintptr_t)slab);
    pmm_free_frames_contiguous(phys, (size_t)pages);
}

/* ── Unlink slab from whatever cache list it's in, then link to new list ── */

static void slab_relink(struct kmem_cache *cache, struct slab *slab,
                        enum slab_state new_state) {
    /* Unlink from current list */
    struct slab **list = NULL;
    switch (slab->state) {
        case SLAB_FULL:    list = &cache->slabs_full;    break;
        case SLAB_PARTIAL: list = &cache->slabs_partial; break;
        case SLAB_FREE:    list = &cache->slabs_free;    break;
        default:
            break;
    }
    if (list) {
        if (slab->prev) slab->prev->next = slab->next;
        else *list = slab->next;
        if (slab->next) slab->next->prev = slab->prev;
    }
    slab->next = slab->prev = NULL;

    /* Link to new list */
    switch (new_state) {
        case SLAB_FULL:
            slab->next = cache->slabs_full;
            if (cache->slabs_full) cache->slabs_full->prev = slab;
            cache->slabs_full = slab;
            break;
        case SLAB_PARTIAL:
            slab->next = cache->slabs_partial;
            if (cache->slabs_partial) cache->slabs_partial->prev = slab;
            cache->slabs_partial = slab;
            break;
        case SLAB_FREE:
            slab->next = cache->slabs_free;
            if (cache->slabs_free) cache->slabs_free->prev = slab;
            cache->slabs_free = slab;
            break;
        default:
            break;
    }
    slab->state = new_state;
}

/* ── Helper: compute slab size and order for a given object size ─────── */

/**
 * slab_sizing - Compute slab geometry for a given object size.
 * @obj_size: Requested object size (includes redzone overhead already).
 * @out_order: On success, receives the slab page order (2^order pages per slab).
 * @out_num: On success, receives the number of objects per slab.
 *
 * Return: 0 on success, or -EINVAL if @obj_size is too large to fit in a
 * single-page slab.  Multi-page slabs are rejected because the object-to-slab
 * reverse mapping relies on a PAGE_SIZE-aligned slab base.  Callers should
 * refuse cache creation and fall back to the page/heap allocator.
 */
static int slab_sizing(size_t obj_size, int *out_order, int *out_num) {
    /* Align object size to the minimum alignment (16 bytes for cacheline safety) */
    size_t aligned = (obj_size + 15) & ~15ULL;
    if (aligned < 16) aligned = 16;

    size_t header = sizeof(struct slab);
    size_t usable = PAGE_SIZE - header;
    int num = (int)(usable / aligned);

    if (num < 1) {
        /* Object too large for a single-page slab.  Multi-page slabs are
         * forbidden because the slab-header address computation
         *
         *   (struct slab *)((uint64_t)obj & ~(slab_size - 1))
         *
         * used in cpu_slab_drain, kmem_cache_free (fallback),
         * kmem_cache_destroy, and slab_cpu_offline relies on the slab
         * base being slab_size-aligned.  PMM (pmm_alloc_frames) only
         * guarantees PAGE_SIZE (4K) alignment, not multi-page alignment;
         * with a multi-page slab at an unaligned base, objects in pages
         * 2+ map to the wrong address, treat object data as a slab header,
         * and corrupt the freelist / free_count / slab lists (type confusion).
         *
         * The caller should reject this cache creation and fall back to
         * the page allocator or heap allocator for oversized objects. */
        return -EINVAL;
    }
    *out_order = 0;
    *out_num   = num;
    return 0;
}

/* ── Create a new slab and add it to the cache's free list ───────────── */

/**
 * slab_grow - Allocate a new slab and add it to the cache's partial list.
 * @cache: The cache to grow.
 * @gfp_flags: GFP flags propagated to the page allocator (GFP_KERNEL, GFP_ATOMIC, ...).
 *
 * Allocates 2^gfporder contiguous pages, initialises the slab header, and
 * populates the free object list (optionally applying object coloring to
 * spread objects across distinct cache-line alignments).
 *
 * Return: 0 on success, or -ENOMEM if the page allocation fails.
 */
static int slab_grow(struct kmem_cache *cache, int gfp_flags) {
    size_t slab_size = PAGE_SIZE * (1ULL << cache->gfporder);
    size_t aligned   = (cache->obj_size + 15) & ~15ULL;
    if (aligned < 16) aligned = 16;
    size_t header    = sizeof(struct slab);

    /* Allocate physically-contiguous pages via the GFP-aware page allocator.
     * Use alloc_pages() so caller-provided flags (GFP_KERNEL, GFP_ATOMIC,
     * GFP_ZERO, etc.) propagate to the physical page allocator.
     * Fall back to raw pmm_alloc_frames() if alloc_pages() returns 0
     * (which can happen before page_allocator_ext_init() in early boot). */
    int pages = 1U << cache->gfporder;
    uint64_t phys_base = alloc_pages(gfp_flags, cache->gfporder);
    if (!phys_base)
        phys_base = (uint64_t)pmm_alloc_frames(pages);
    if (!phys_base)
        return -ENOMEM;
    void *virt = PHYS_TO_VIRT(phys_base);
    memset(virt, 0, slab_size);

    /* Set up slab header */
    struct slab *slab = (struct slab *)virt;
    slab->total    = cache->num;
    slab->free_count = cache->num;
    slab->free_list = NULL;
    slab->next     = NULL;
    slab->prev     = NULL;
    slab->state    = SLAB_PARTIAL;

    /* Build an array of object pointers for shuffling */
    /* ── Object coloring: shift first object by a cycling offset to avoid
     *     cache-line ping-pong (false sharing) between different slabs. ── */
    int colour_bytes = 0;
    if (cache->colour_off > 0) {
        int colour_step  = 16;  /* minimum alignment step */
        int colour_count = cache->colour_off / colour_step + 1;
        int colour = cache->colour_next;
        cache->colour_next = (colour + 1) % colour_count;
        colour_bytes = colour * colour_step;
        if (colour_bytes > cache->colour_off)
            colour_bytes = cache->colour_off;
    }
    void *obj_base = (uint8_t *)virt + header + colour_bytes;
    void **obj_ptrs = (void **)kmalloc(sizeof(void *) * (size_t)cache->num);
    if (!obj_ptrs) {
        /* Fall back to sequential order if we can't allocate the temp array.
         * NOTE: constructor is called BEFORE setting free-list pointer so the
         * constructor cannot corrupt the free list (bytes 0-7).  After the
         * free-list pointer is set, poison bytes 8+ so that alloc-time UAF
         * verification works on freshly-grown slabs. */
        for (int i = 0; i < cache->num; i++) {
            void *obj = (uint8_t *)obj_base + (size_t)i * aligned;
            if (cache->ctor)
                cache->ctor(obj);
            *(void **)obj = slab->free_list;
            slab->free_list = obj;
            /* Poison bytes 8+ for alloc-time UAF poison verification */
            slab_poison_free(cache, obj);
        }
        slab_relink(cache, slab, SLAB_PARTIAL);
        return 0;
    }

    /* Collect all object pointers */
    for (int i = 0; i < cache->num; i++) {
        obj_ptrs[i] = (uint8_t *)obj_base + (size_t)i * aligned;
        if (cache->ctor)
            cache->ctor(obj_ptrs[i]);
    }

    /* Fisher-Yates shuffle using kernel RNG */
    for (int i = cache->num - 1; i > 0; i--) {
        int j = (int)(rng_get_u32() % (uint32_t)(i + 1));
        void *tmp = obj_ptrs[i];
        obj_ptrs[i] = obj_ptrs[j];
        obj_ptrs[j] = tmp;
    }

    /* Build free list from shuffled array and poison for UAF detection */
    for (int i = 0; i < cache->num; i++) {
        *(void **)obj_ptrs[i] = slab->free_list;
        slab->free_list = obj_ptrs[i];
        /* Poison bytes 8+ so alloc-time UAF check can verify poison is intact */
        slab_poison_free(cache, obj_ptrs[i]);
    }

    kfree(obj_ptrs);

    /* Link into cache's partial list via slab_relink */
    slab_relink(cache, slab, SLAB_PARTIAL);
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

/**
 * cpu_slab_refill - Refill the current CPU's object cache from the slab freelist.
 * @cache: The cache to refill from.
 * @gfp_flags: GFP flags passed through to slab_grow if a new slab is needed.
 *
 * Caller must hold @cache->lock with IRQs disabled.
 *
 * A note on ordering: the per-CPU cache must be checked *first* because an
 * interrupt handler may have fast-path freed an object into it during the IRQ
 * window between the fast-path check in kmem_cache_alloc() and acquiring the
 * cache lock.  Resetting count unconditionally would leak that object.
 *
 * Return: A freshly obtained object for immediate use, or NULL on OOM.  Any
 * surplus objects are stashed in the caller's per-CPU cache.
 */
static void *cpu_slab_refill(struct kmem_cache *cache, int gfp_flags) {
    int cpu = smp_get_cpu_id();
    struct cpu_slab *cpu_s = &cache->cpu_slab[cpu];
    void *ret = NULL;

    /* Preserve any objects that arrived via fast-path free during the IRQ
     * window.  Take one for immediate use; leave the rest cached so we
     * don't need to refill from slabs at all. */
    if (cpu_s->count > 0) {
        cpu_s->count--;
        ret = cpu_s->objects[cpu_s->count];
        if (cpu_s->count > 0)
            return ret;     /* cpu cache already has extras, skip refill */
    }
    cpu_s->count = 0;

    if (ret)
        return ret;

    /* Pull objects from partial slabs first, then free slabs, then grow */
    while (cpu_s->count < SLAB_CPU_CACHE_SIZE) {
        void *obj = NULL;
        struct slab *slab;

        /* Try partial slabs first */
        slab = cache->slabs_partial;
        if (slab) {
            obj = slab->free_list;
            if (obj) {
                slab->free_list = *(void **)obj;
                slab->free_count--;
                if (slab->free_count == 0)
                    slab_relink(cache, slab, SLAB_FULL);
            }
        }

        /* If no partial, try a free slab */
        if (!obj && cache->slabs_free) {
            slab = cache->slabs_free;
            slab_relink(cache, slab, SLAB_PARTIAL);
            obj = slab->free_list;
            if (obj) {
                slab->free_list = *(void **)obj;
                slab->free_count--;
                if (slab->free_count == 0)
                    slab_relink(cache, slab, SLAB_FULL);
            }
        }

        /* If still nothing, grow a new slab */
        if (!obj && slab_grow(cache, gfp_flags) == 0) {
            slab = cache->slabs_partial;
            if (slab) {
                obj = slab->free_list;
                if (obj) {
                    slab->free_list = *(void **)obj;
                    slab->free_count--;
                    if (slab->free_count == 0)
                        slab_relink(cache, slab, SLAB_FULL);
                }
            }
        }

        if (!obj) break; /* truly out of memory */

        /* The first object is returned to the caller */
        if (!ret) {
            ret = obj;
        } else {
            /* Subsequent objects go into the per-CPU cache */
            cpu_s->objects[cpu_s->count++] = obj;
        }
    }

    return ret;
}

/**
 * cpu_slab_drain - Drain the current CPU's object cache back into the slab freelist.
 * @cache: The cache whose per-CPU cache is to be emptied.
 *
 * Caller must hold @cache->lock with IRQs disabled.  Objects are returned to
 * their slabs' free lists at a random depth (see slab_freelist_insert_random)
 * to scramble allocation order for heap-exploit hardening.
 */
static void cpu_slab_drain(struct kmem_cache *cache) {
    int cpu = smp_get_cpu_id();
    struct cpu_slab *cpu_s = &cache->cpu_slab[cpu];

    while (cpu_s->count > 0) {
        cpu_s->count--;
        void *obj = cpu_s->objects[cpu_s->count];

        /* Find which slab this object belongs to */
        size_t slab_size = PAGE_SIZE * (1ULL << cache->gfporder);
        struct slab *slab = (struct slab *)((uint64_t)obj & ~(slab_size - 1));

        /* Insert object into slab free list at a random depth
         * to scramble allocation order (heap exploit hardening). */
        slab_freelist_insert_random(slab, obj);
        slab->free_count++;

        /* Update slab list position */
        if (slab->free_count == 1) {
            slab_relink(cache, slab, SLAB_PARTIAL);
        } else if (slab->free_count == slab->total) {
            slab_relink(cache, slab, SLAB_FREE);
        }
    }
}

/**
 * kmem_cache_create - Create a cache for fixed-size objects.
 * @name: Human-readable name for the cache (used in diagnostics; may be NULL).
 * @obj_size: Size in bytes of each object.
 * @align: Desired object alignment (power of two).  Zero selects a default of 16;
 *         the value is clamped to at least 8 so the redzone canary stays aligned.
 * @ctor: Constructor invoked once per freshly allocated slab object; may be NULL.
 *
 * On success the cache pre-allocates one slab and registers itself in the
 * global cache list so it is visible to slab_get_stats() and the reaper.
 *
 * Return: A pointer to the new cache, or NULL on invalid input, overflow, or
 * out-of-memory.  A cache larger than a single page can hold is rejected.
 */
struct kmem_cache *kmem_cache_create(const char *name, size_t obj_size,
                                     size_t align, kmem_cache_ctor_t ctor) {
    if (obj_size == 0) {
        kprintf("[SLAB] ERROR: kmem_cache_create('%s') with size=0 is invalid\n",
                name ? name : "(null)");
        return NULL;
    }

    struct kmem_cache *cache = (struct kmem_cache *)kmalloc(sizeof(struct kmem_cache));
    if (!cache) return NULL;

    memset(cache, 0, sizeof(*cache));
    cache->name      = name;
    cache->align     = (align == 0) ? 16 : align;
    /* Ensure alignment is a power of two (requirement of the documented API) */
    if (cache->align & (cache->align - 1))
        cache->align = 16;
    /* Minimum alignment of 8 for redzone canary (uint64_t) alignment.
     * Smaller alignments would allow the redzone to be placed at a
     * sub-8-byte-aligned offset, causing a misaligned uint64_t access. */
    if (cache->align < 8)
        cache->align = 8;
    /* Round up user size to the requested alignment so that each object's
     * user-data region starts at a correctly-aligned offset within the slab.
     * This is what callers expect when they pass a non-zero align value.
     * Check for wrap-around since obj_size is caller-controlled. */
    size_t cache_obj_size_rounded = (obj_size + cache->align - 1) & ~(size_t)(cache->align - 1);
    if (cache_obj_size_rounded < obj_size) {
        kprintf("[SLAB] ERROR: kmem_cache_create('%s') alignment rounding overflow (size=%zu, align=%zu)\n",
                name ? name : "(null)", obj_size, cache->align);
        kfree(cache);
        return NULL;
    }
    obj_size = cache_obj_size_rounded;
    cache->user_size = obj_size;
    cache->ctor      = ctor;

    /* Expand the internal object size to include the redzone canary.
     * Check for wrap-around since obj_size is caller-controlled. */
    obj_size += SLAB_REDZONE_SIZE;
    if (obj_size < cache->user_size) {
        kfree(cache);
        return NULL;
    }

    if (slab_sizing(obj_size, &cache->gfporder, &cache->num) != 0) {
        kprintf("[SLAB] ERROR: kmem_cache_create('%s') object size %zu too large for slab\n",
                name ? name : "(null)", obj_size);
        kfree(cache);
        return NULL;
    }
    cache->obj_size = (obj_size + 15) & ~15ULL;
    if (cache->obj_size < 16) cache->obj_size = 16;

    /* Calculate colour offset for object coloring (cache-line ping-pong avoidance).
     * colour_off = leftover bytes in the slab after fitting all objects.
     * Each new slab gets a different colour offset, spreading objects across
     * distinct cache-line alignments across slabs. */
    {
        size_t slab_sz = PAGE_SIZE * (1ULL << cache->gfporder);
        size_t hdr_sz  = sizeof(struct slab);
        size_t al_sz   = (cache->obj_size + 15) & ~15ULL;
        if (al_sz < 16) al_sz = 16;
        size_t used    = (size_t)cache->num * al_sz;
        size_t avail   = slab_sz - hdr_sz;
        if (avail > used)
            cache->colour_off = (int)(avail - used);
        else
            cache->colour_off = 0;
        cache->colour_next = 0;
    }

    spinlock_init(&cache->lock);

    /* Pre-allocate one slab so the cache is immediately usable */
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cache->lock, &irq_flags);
    slab_grow(cache, GFP_KERNEL);
    spinlock_irqsave_release(&cache->lock, irq_flags);

    /* Register in global cache list (list lock protects against
     * concurrent iteration by slab_get_stats / kmem_cache_reap). */
    uint64_t list_irq_flags;
    spinlock_irqsave_acquire(&cache_list_lock, &list_irq_flags);
    cache->next = cache_list;
    cache_list = cache;
    spinlock_irqsave_release(&cache_list_lock, list_irq_flags);

    return cache;
}

/**
 * kmem_cache_alloc - Allocate an object from a cache.
 * @cache: The cache to allocate from.
 * @gfp_flags: GFP flags propagated to the page allocator when the slab grows.
 *
 * Serves the fast path from the caller's per-CPU object cache without taking
 * the cache lock; falls back to cpu_slab_refill() under lock when the cache
 * is empty.  The returned object is zero-free-poison-checked for UAF, freshly
 * poisoned, redzoned, and tracked by KASAN and kmemleak.
 *
 * Return: A pointer to the allocated object, or NULL on OOM or an invalid cache.
 */
void *kmem_cache_alloc(struct kmem_cache *cache, int gfp_flags) {
    /* Validate that the cache is valid and initialized.
     * A NULL cache pointer or a cache with obj_size == 0 indicates
     * a bug in the caller (double-destroyed or never-initialized cache). */
    if (!cache || cache->obj_size == 0) {
        kprintf("[SLAB] ERROR: kmem_cache_alloc on invalid cache "
                "(NULL or uninitialized, gfp_flags=%d)\n", gfp_flags);
        return NULL;
    }

    int cpu = smp_get_cpu_id();
    struct cpu_slab *cpu_s = &cache->cpu_slab[cpu];

    /* ── Fast path: try per-CPU object cache first ── */
    uint64_t irq_save;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(irq_save) : : "memory");

    if (cpu_s->count > 0) {
        cpu_s->count--;
        void *obj = cpu_s->objects[cpu_s->count];
        if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");

        /* Verify free poison is intact for UAF detection */
        slab_check_poison_free(cache, obj);

        /* Poison with allocation pattern and set redzone */
        slab_poison_alloc(cache, obj);
        slab_set_redzone(cache, obj);

        /* KASAN: mark user area accessible, mark redzone as poisoned */
        kasan_unpoison(obj, cache->user_size);
        kasan_poison_redzone((uint8_t *)obj + cache->user_size,
                             cache->obj_size - cache->user_size);

        /* kmemleak: track this slab allocation */
        kmemleak_alloc(obj, cache->user_size, KMEMLEAK_SLAB);

        return obj;
    }

    if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");

    /* ── Slow path: refill from slab under lock ── */
    uint64_t lock_flags;
    spinlock_irqsave_acquire(&cache->lock, &lock_flags);

    void *obj = cpu_slab_refill(cache, gfp_flags);

    spinlock_irqsave_release(&cache->lock, lock_flags);

    if (obj) {
        /* Verify free poison is intact for UAF detection */
        slab_check_poison_free(cache, obj);

        slab_poison_alloc(cache, obj);
        slab_set_redzone(cache, obj);

        /* KASAN: mark user area accessible, mark redzone as poisoned */
        kasan_unpoison(obj, cache->user_size);
        kasan_poison_redzone((uint8_t *)obj + cache->user_size,
                             cache->obj_size - cache->user_size);

        /* kmemleak: track this slab allocation */
        kmemleak_alloc(obj, cache->user_size, KMEMLEAK_SLAB);
    }
    return obj;
}

/**
 * kmem_cache_free - Return an object to its cache.
 * @cache: The cache the object was allocated from.
 * @obj: Pointer to the object to free.
 *
 * The object must have been allocated from @cache.  The redzone is checked
 * first for double-free detection; the object is then KASAN-poisoned, untracked
 * from kmemleak, and returned to the per-CPU cache (fast path) or drained back
 * into its slab's freelist under lock (slow path).
 *
 * Return: None; an invalid cache or object is logged and otherwise ignored.
 */
void kmem_cache_free(struct kmem_cache *cache, void *obj) {
    if (!obj || !cache) return;

    /* Validate that the cache has a sane object size.  A cache whose
     * obj_size is zero was never properly initialized (or was already
     * destroyed but the pointer is stale — a caching bug). */
    if (cache->obj_size == 0) {
        kprintf("[SLAB] ERROR: kmem_cache_free on invalid cache "
                "(obj_size=0, obj=%p)\n", obj);
        return;
    }

    /* Check redzone before modifying the object.
     * On double-free, the redzone has been poisoned by the first free's
     * slab_poison_free — detect this and refuse to proceed. */
    if (!slab_check_redzone(cache, obj)) {
        kprintf("[SLAB] DOUBLE-FREE DETECTED in '%s': obj=%p — refusing to free\n",
                cache->name, obj);
        return;
    }

    /* KASAN: verify user area hasn't been touched and poison entire object */
    kasan_check(obj, cache->user_size, 0);
    kasan_poison(obj, cache->obj_size);

    /* kmemleak: stop tracking this slab allocation */
    kmemleak_free(obj);

    /* Poison the object with the free pattern (reserving first 8 bytes) */
    slab_poison_free(cache, obj);

    int cpu = smp_get_cpu_id();
    struct cpu_slab *cpu_s = &cache->cpu_slab[cpu];

    /* ── Fast path: push to per-CPU cache if room ── */
    uint64_t irq_save;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(irq_save) : : "memory");

    if (cpu_s->count < SLAB_CPU_CACHE_SIZE) {
        cpu_s->objects[cpu_s->count++] = obj;
        if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");
        return;
    }

    if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");

    /* ── Slow path: drain cache to slabs under lock ── */
    uint64_t lock_flags;
    spinlock_irqsave_acquire(&cache->lock, &lock_flags);

    cpu_slab_drain(cache);

    /* Now add the new object (cache should have room after drain) */
    if (cpu_s->count < SLAB_CPU_CACHE_SIZE) {
        cpu_s->objects[cpu_s->count++] = obj;
    } else {
        /* Fallback: direct to slab if drain didn't clear enough space */
        size_t slab_size = PAGE_SIZE * (1ULL << cache->gfporder);
        struct slab *slab = (struct slab *)((uint64_t)obj & ~(slab_size - 1));

        slab_freelist_insert_random(slab, obj);
        slab->free_count++;

        if (slab->free_count == 1) {
            slab_relink(cache, slab, SLAB_PARTIAL);
        } else if (slab->free_count == slab->total) {
            slab_relink(cache, slab, SLAB_FREE);
        }
    }

    spinlock_irqsave_release(&cache->lock, lock_flags);
}

/**
 * kmem_cache_destroy - Destroy a cache and free all its slabs.
 * @cache: The cache to tear down.
 *
 * Unregisters the cache from the global list, drains every CPU's per-CPU cache
 * back into the slab freelists, and returns all slab pages to the page
 * allocator before freeing the cache descriptor itself.
 *
 * Return: None.  Only safe when every object from the cache has been freed.
 */
void kmem_cache_destroy(struct kmem_cache *cache) {
    if (!cache) return;

    int pages = 1U << cache->gfporder;
    size_t slab_size = PAGE_SIZE * (1ULL << cache->gfporder);

    /* Remove from global cache list first, so no other thread can
     * find this cache while we tear it down. */
    uint64_t list_irq_flags;
    spinlock_irqsave_acquire(&cache_list_lock, &list_irq_flags);
    struct kmem_cache **pp = &cache_list;
    while (*pp) {
        if (*pp == cache) {
            *pp = cache->next;
            break;
        }
        pp = &(*pp)->next;
    }
    spinlock_irqsave_release(&cache_list_lock, list_irq_flags);

    /* Drain per-CPU caches before freeing slabs.  Objects in per-CPU
     * caches have been removed from their slab's free list (their memory
     * lives inside the slab pages) but have not yet been returned to the
     * slab freelist.  If we skip this step and free slab pages directly,
     * the per-CPU cache entries become dangling pointers inside the
     * about-to-be-freed cache structure.  Return every cached object to
     * its slab's free list and increment free_count so the per-CPU cache
     * is empty when we start freeing pages. */
    for (int cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        struct cpu_slab *cpu_s = &cache->cpu_slab[cpu];
        while (cpu_s->count > 0) {
            cpu_s->count--;
            void *obj = cpu_s->objects[cpu_s->count];
            struct slab *slab = (struct slab *)((uint64_t)obj & ~(slab_size - 1));
            *(void **)obj = slab->free_list;
            slab->free_list = obj;
            slab->free_count++;
        }
    }

    /* Now free all slabs (no other thread can find this cache anymore) */

    /* Free all slabs across all three lists */
    struct slab *slab = cache->slabs_full;
    while (slab) {
        struct slab *nxt = slab->next;
        slab_free_pages(slab, pages);
        slab = nxt;
    }
    slab = cache->slabs_partial;
    while (slab) {
        struct slab *nxt = slab->next;
        slab_free_pages(slab, pages);
        slab = nxt;
    }
    slab = cache->slabs_free;
    while (slab) {
        struct slab *nxt = slab->next;
        slab_free_pages(slab, pages);
        slab = nxt;
    }

    kfree(cache);
}


/* ── Built-in caches ─────────────────────────────────────────────────── */

/**
 * slab_init - Initialise the slab subsystem.
 *
 * Idempotently marks the slab subsystem as initialised.  No built-in caches
 * are currently created at boot because process and socket structures use
 * static tables, and kobject/inode/dentry types are not yet defined.  When
 * those types gain dynamic allocators, their caches should be created here
 * with sizes validated by BUILD_BUG_ON.
 *
 * Return: None.
 */
void __init slab_init(void) {
    if (slab_initialized) return;

    kprintf("[..] Initializing slab allocator...\n");

    /* NOTE: slab caches for kobject, inode, dentry, process, socket, and
     * process_fd are not yet created because:
     *   - This kernel uses a static process_table[] for struct process
     *     (7024 bytes) and a static socket_table[] for struct socket
     *     (256 bytes) rather than slab-backed allocators.
     *   - There are no struct kobject, struct inode, or struct dentry
     *     types defined in this kernel — the VFS layer is path-based
     *     and sysfs uses a simple entry table, not a kobject tree.
     *   When these types gain their own dynamic allocators, their slab
     *   caches should be created here with the correct sizes verified
     *   by BUILD_BUG_ON(sizeof(struct...) != cache->obj_size).
     */

    slab_initialized = 1;
}

/* ── Stub: slab_create ─────────────────────────────── */
static int slab_create(const char *name, size_t size, unsigned long align, void *ctor)
{
    (void)name;
    (void)size;
    (void)align;
    (void)ctor;
    kprintf("[slab] slab_create: not yet implemented\n");
    return 0;
}
/* ── Stub: slab_destroy ─────────────────────────────── */
static int slab_destroy(void *cache)
{
    (void)cache;
    kprintf("[slab] slab_destroy: not yet implemented\n");
    return 0;
}
/* ── Stub: slab_alloc ─────────────────────────────── */
static void* slab_alloc(void *cache, int flags)
{
    (void)cache;
    (void)flags;
    kprintf("[slab] slab_alloc: not yet implemented\n");
    return 0;
}
/* ── Stub: slab_free ─────────────────────────────── */
static int slab_free(void *cache, void *obj)
{
    (void)cache;
    (void)obj;
    kprintf("[slab] slab_free: not yet implemented\n");
    return 0;
}
/**
 * slab_get_stats - Collect aggregate statistics across every registered cache.
 * @s: Out-parameter filled with the aggregated totals.
 *
 * Walks the global cache list under the list lock and per-cache locks, summing
 * object counts and memory used across all full/partial/free slabs.
 *
 * Return: None; @s is zeroed and filled in place (a NULL pointer is ignored).
 */
void slab_get_stats(struct slab_stats *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    uint64_t list_irq_flags;
    spinlock_irqsave_acquire(&cache_list_lock, &list_irq_flags);
    struct kmem_cache *cache = cache_list;
    while (cache) {
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&cache->lock, &irq_flags);
        s->cache_count++;
        size_t slab_size = PAGE_SIZE * (1ULL << cache->gfporder);
        /* Count objects across all slabs */
        int total_in_cache = 0;
        int free_in_cache = 0;
        struct slab *slab;
        slab = cache->slabs_full;
        while (slab) { total_in_cache += slab->total; free_in_cache += slab->free_count; s->memory_used += slab_size; slab = slab->next; }
        slab = cache->slabs_partial;
        while (slab) { total_in_cache += slab->total; free_in_cache += slab->free_count; s->memory_used += slab_size; slab = slab->next; }
        slab = cache->slabs_free;
        while (slab) { total_in_cache += slab->total; free_in_cache += slab->free_count; s->memory_used += slab_size; slab = slab->next; }
        s->total_objects += (uint64_t)total_in_cache;
        s->used_objects += (uint64_t)(total_in_cache - free_in_cache);
        spinlock_irqsave_release(&cache->lock, irq_flags);
        cache = cache->next;
    }
    spinlock_irqsave_release(&cache_list_lock, list_irq_flags);
}
/**
 * kmem_cache_reap - Return all empty (completely free) slabs to the page allocator.
 *
 * Called by the kernel when memory is tight.  Walks the global cache list and,
 * for each cache, uses a trylock so it cannot self-deadlock when invoked from
 * the slab allocation slow path (via pmm_alloc_frame -> reclaim) which already
 * holds the current cache's non-recursive lock.  Empty slabs skipped this call
 * are freed on a subsequent reap from a non-recursive context.
 *
 * Return: None.
 */
void kmem_cache_reap(void) {
    uint64_t list_irq_flags;
    spinlock_irqsave_acquire(&cache_list_lock, &list_irq_flags);
    struct kmem_cache *cache = cache_list;
    while (cache) {
        /*
         * Use trylock to avoid deadlock: this function may be called from
         * the slab allocation slow path (via pmm_alloc_frame → reclaim)
         * which already holds the current cache's lock (cache->lock).
         * A blocking spinlock_irqsave_acquire would self-deadlock since
         * the spinlock is non-recursive.  Skipping is safe — the missed
         * empty slabs will be freed on a subsequent reap call from a
         * non-recursive context.
         *
         * IRQs are already disabled by the cache_list_lock irqsave
         * acquisition above, so plain spinlock_try_acquire is sufficient.
         */
        if (spinlock_try_acquire(&cache->lock)) {
            int pages = 1U << cache->gfporder;
            struct slab *slab = cache->slabs_free;
            while (slab) {
                struct slab *nxt = slab->next;
                /* slab_relink would also work, but we skip the relink since we're freeing */
                if (slab->prev) slab->prev->next = slab->next;
                else cache->slabs_free = slab->next;
                if (slab->next) slab->next->prev = slab->prev;
                slab_free_pages(slab, pages);
                slab = nxt;
            }
            spinlock_release(&cache->lock);
        }
        cache = cache->next;
    }
    spinlock_irqsave_release(&cache_list_lock, list_irq_flags);
}

/* ── CPU hotplug: drain per-CPU slab cache ───────────────────────────── */

/**
 * slab_cpu_offline - Drain the per-CPU slab cache for a CPU across all caches.
 * @cpu_id: The CPU being taken offline.
 *
 * Called from cpuhp_take_cpu_offline() once the target CPU has been stopped
 * (scheduler disabled, tasks migrated), so no new objects can reach its
 * per-CPU cache during the drain.  Every cached object is returned to its
 * slab's freelist.
 *
 * Return: The number of objects drained, or -EINVAL on an out-of-range @cpu_id.
 */
int slab_cpu_offline(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= SMP_MAX_CPUS)
        return -EINVAL;

    int drained = 0;

    uint64_t list_irq_flags;
    spinlock_irqsave_acquire(&cache_list_lock, &list_irq_flags);

    struct kmem_cache *cache = cache_list;
    while (cache) {
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&cache->lock, &irq_flags);

        struct cpu_slab *cpu_s = &cache->cpu_slab[cpu_id];
        size_t slab_size = PAGE_SIZE * (1ULL << (unsigned int)cache->gfporder);

        while (cpu_s->count > 0) {
            cpu_s->count--;
            void *obj = cpu_s->objects[cpu_s->count];
            struct slab *slab = (struct slab *)((uint64_t)(uintptr_t)obj & ~(slab_size - 1));

            /* Return the object to its slab's freelist (scrambled order) */
            slab_freelist_insert_random(slab, obj);
            slab->free_count++;

            /* Update slab list position if its free status changed */
            if (slab->free_count == 1) {
                slab_relink(cache, slab, SLAB_PARTIAL);
            } else if (slab->free_count == slab->total) {
                slab_relink(cache, slab, SLAB_FREE);
            }

            drained++;
        }

        spinlock_irqsave_release(&cache->lock, irq_flags);
        cache = cache->next;
    }

    spinlock_irqsave_release(&cache_list_lock, list_irq_flags);
    return drained;
}

/**
 * slab_cpu_online - Clear the per-CPU slab cache for a CPU across all caches.
 * @cpu_id: The CPU being brought back online.
 *
 * Called from cpuhp_bring_cpu() when a CPU comes back online.  The cache is
 * expected to already be empty (drained by slab_cpu_offline()) but is cleared
 * for safety, analogous to pmm_cpu_online().  No lock is taken: the target
 * CPU is not yet online and the list is stable under the caller's cpuhp_lock.
 *
 * Return: None.
 */
void slab_cpu_online(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= SMP_MAX_CPUS)
        return;

    /* No lock: the target CPU is not yet online and no other code
     * accesses this CPU's per-CPU slot during bring-up.  The cache_list
     * iteration is safe because the caller (cpuhp_bring_cpu) holds
     * cpuhp_lock which serialises against concurrent hotplug operations
     * that could modify the list. */
    struct kmem_cache *cache = cache_list;
    while (cache) {
        cache->cpu_slab[cpu_id].count = 0;
        cache = cache->next;
    }
}
