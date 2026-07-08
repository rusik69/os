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

/*
 * Slab allocator — O(1) allocate/free for fixed-size kernel objects.
 *
 * Each cache manages a set of slabs (contiguous physical pages). Objects are
 * carved from slabs and tracked via an intrusive free list stored in freed
 * objects (no bitmap overhead). Slabs move between three lists:
 *
 *   slabs_partial  — some objects free, some allocated (preferred for alloc)
 *   slabs_full     — all objects allocated
 *   slabs_free     — all objects free (candidate for reaping)
 *
 * Per-CPU object cache (cpu_slab) provides a lockless fast path for the
 * common case, avoiding contention on the cache spinlock on SMP systems.
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

/* Per-CPU slab cache — small array of objects for lockless fast path */
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

struct slab {
    struct slab    *next;          /* linked list in cache */
    struct slab    *prev;
    void           *free_list;     /* head of free object linked list */
    int             free_count;    /* number of free objects in this slab */
    int             total;         /* total objects in this slab */
    enum slab_state state;         /* which list this slab is linked into */
};

/* ── Cache descriptor ────────────────────────────────────────────────── */

struct kmem_cache {
    const char       *name;
    size_t            obj_size;   /* actual object size (rounded + aligned), includes redzone */
    size_t            user_size;  /* caller-requested object size (without redzone) */
    size_t            align;      /* requested alignment */
    int               gfporder;   /* 2^gfporder pages per slab */
    int               num;        /* objects per slab */
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

/* ── Slab statistics ────────────────────────────────────────────────── */

void slab_get_stats(struct slab_stats *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
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
}

/* ── Helper: free a physical page (given kernel virtual address) ──────── */
static void slab_page_free(void *virt) {
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)virt));
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

static void slab_sizing(size_t obj_size, int *out_order, int *out_num) {
    /* Align object size to the minimum alignment (16 bytes for cacheline safety) */
    size_t aligned = (obj_size + 15) & ~15ULL;
    if (aligned < 16) aligned = 16;

    int order = 0;
    size_t slab_size = PAGE_SIZE;          /* 2^0 = 1 page */
    size_t header = sizeof(struct slab);
    int num;

    for (;;) {
        size_t usable = slab_size - header;
        num = (int)(usable / aligned);
        if (num >= 8 || order >= 4) break; /* at least 8 objects, max 16 pages */
        order++;
        slab_size = PAGE_SIZE * (1ULL << order);
    }

    if (num < 1) num = 1;
    *out_order = order;
    *out_num   = num;
}

/* ── Create a new slab and add it to the cache's free list ───────────── */

static int slab_grow(struct kmem_cache *cache) {
    size_t slab_size = PAGE_SIZE * (1ULL << cache->gfporder);
    size_t aligned   = (cache->obj_size + 15) & ~15ULL;
    if (aligned < 16) aligned = 16;
    size_t header    = sizeof(struct slab);

    /* Allocate contiguous pages via PMM */
    uint64_t phys_base = 0;
    int pages = 1U << cache->gfporder;
    for (int i = 0; i < pages; i++) {
        uint64_t p = pmm_alloc_frame();
        if (!p) {
            for (int j = 0; j < i; j++)
                pmm_free_frame(phys_base + (uint64_t)j * PAGE_SIZE);
            return -ENOMEM;
        }
        if (i == 0) phys_base = p;
    }
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
    void *obj_base = (uint8_t *)virt + header;
    void **obj_ptrs = (void **)kmalloc(sizeof(void *) * (size_t)cache->num);
    if (!obj_ptrs) {
        /* Fall back to sequential order if we can't allocate the temp array */
        for (int i = 0; i < cache->num; i++) {
            void *obj = (uint8_t *)obj_base + (size_t)i * aligned;
            *(void **)obj = slab->free_list;
            slab->free_list = obj;
            if (cache->ctor)
                cache->ctor(obj);
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

    /* Build free list from shuffled array */
    for (int i = 0; i < cache->num; i++) {
        *(void **)obj_ptrs[i] = slab->free_list;
        slab->free_list = obj_ptrs[i];
    }

    kfree(obj_ptrs);

    /* Link into cache's partial list via slab_relink */
    slab_relink(cache, slab, SLAB_PARTIAL);
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

/* Refill the current CPU's object cache from the slab freelist.
 * Must be called with cache->lock held and IRQs disabled.
 * Returns one object for immediate use, and fills cpu_slab with extras. */
static void *cpu_slab_refill(struct kmem_cache *cache) {
    int cpu = smp_get_cpu_id();
    struct cpu_slab *cpu_s = &cache->cpu_slab[cpu];
    void *ret = NULL;

    /* Reset local cache */
    cpu_s->count = 0;

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
        if (!obj && slab_grow(cache) == 0) {
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

/* Drain the current CPU's object cache back into the slab freelist.
 * Must be called with cache->lock held and IRQs disabled. */
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

struct kmem_cache *kmem_cache_create(const char *name, size_t obj_size,
                                     size_t align, kmem_cache_ctor_t ctor) {
    struct kmem_cache *cache = (struct kmem_cache *)kmalloc(sizeof(struct kmem_cache));
    if (!cache) return NULL;

    memset(cache, 0, sizeof(*cache));
    cache->name      = name;
    cache->align     = (align == 0) ? 16 : align;
    cache->ctor      = ctor;
    cache->user_size = obj_size;

    /* Expand the internal object size to include the redzone canary */
    obj_size += SLAB_REDZONE_SIZE;

    slab_sizing(obj_size, &cache->gfporder, &cache->num);
    cache->obj_size = (obj_size + 15) & ~15ULL;
    if (cache->obj_size < 16) cache->obj_size = 16;

    spinlock_init(&cache->lock);

    /* Pre-allocate one slab so the cache is immediately usable */
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cache->lock, &irq_flags);
    slab_grow(cache);
    spinlock_irqsave_release(&cache->lock, irq_flags);

    /* Register in global cache list */
    cache->next = cache_list;
    cache_list = cache;

    return cache;
}

void *kmem_cache_alloc(struct kmem_cache *cache) {
    int cpu = smp_get_cpu_id();
    struct cpu_slab *cpu_s = &cache->cpu_slab[cpu];

    /* ── Fast path: try per-CPU object cache first ── */
    uint64_t irq_save;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(irq_save) : : "memory");

    if (cpu_s->count > 0) {
        cpu_s->count--;
        void *obj = cpu_s->objects[cpu_s->count];
        if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");

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

    void *obj = cpu_slab_refill(cache);

    spinlock_irqsave_release(&cache->lock, lock_flags);

    if (obj) {
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

void kmem_cache_free(struct kmem_cache *cache, void *obj) {
    if (!obj || !cache) return;

    /* Check redzone before modifying the object */
    slab_check_redzone(cache, obj);

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

void kmem_cache_destroy(struct kmem_cache *cache) {
    if (!cache) return;

    int pages = 1U << cache->gfporder;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cache->lock, &irq_flags);

    /* Free all slabs across all three lists */
    struct slab *slab = cache->slabs_full;
    while (slab) {
        struct slab *nxt = slab->next;
        for (int p = 0; p < pages; p++)
            slab_page_free((uint8_t *)slab + p * PAGE_SIZE);
        slab = nxt;
    }
    slab = cache->slabs_partial;
    while (slab) {
        struct slab *nxt = slab->next;
        for (int p = 0; p < pages; p++)
            slab_page_free((uint8_t *)slab + p * PAGE_SIZE);
        slab = nxt;
    }
    slab = cache->slabs_free;
    while (slab) {
        struct slab *nxt = slab->next;
        for (int p = 0; p < pages; p++)
            slab_page_free((uint8_t *)slab + p * PAGE_SIZE);
        slab = nxt;
    }

    spinlock_irqsave_release(&cache->lock, irq_flags);

    /* Remove from global cache list */
    struct kmem_cache **pp = &cache_list;
    struct kmem_cache *cur = cache_list;
    while (cur) {
        if (cur == cache) {
            *pp = cur->next;
            break;
        }
        pp = &cur->next;
        cur = cur->next;
    }

    kfree(cache);
}

void kmem_cache_reap(void) {
    struct kmem_cache *cache = cache_list;
    while (cache) {
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&cache->lock, &irq_flags);

        int pages = 1U << cache->gfporder;
        struct slab *slab = cache->slabs_free;
        while (slab) {
            struct slab *nxt = slab->next;
            /* slab_relink would also work, but we skip the relink since we're freeing */
            if (slab->prev) slab->prev->next = slab->next;
            else cache->slabs_free = slab->next;
            if (slab->next) slab->next->prev = slab->prev;
            for (int p = 0; p < pages; p++)
                slab_page_free((uint8_t *)slab + p * PAGE_SIZE);
            slab = nxt;
        }

        spinlock_irqsave_release(&cache->lock, irq_flags);
        cache = cache->next;
    }
}

/* ── Built-in caches ─────────────────────────────────────────────────── */

struct kmem_cache *cache_process = NULL;
struct kmem_cache *cache_socket  = NULL;

void __init slab_init(void) {
    if (slab_initialized) return;

    kprintf("[..] Initializing slab allocator...\n");

    cache_process = kmem_cache_create("process", 280, 0, NULL);
    cache_socket  = kmem_cache_create("socket",   96, 0, NULL);

    if (cache_process)
        kprintf("[OK] Slab: cache_process (%lld-byte objects, %d per slab)\n",
                (unsigned long long)cache_process->obj_size, cache_process->num);
    if (cache_socket)
        kprintf("[OK] Slab: cache_socket (%lld-byte objects, %d per slab)\n",
                (unsigned long long)cache_socket->obj_size, cache_socket->num);

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
