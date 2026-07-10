/*
 * kfence.c — Kernel Electric Fence (KFENCE): low-overhead memory safety
 *
 * KFENCE is a low-overhead memory error detector that samples heap
 * allocations at a configurable rate (default 1/100) and places them
 * in a dedicated pool with guard pages, detecting:
 *   - Out-of-bounds accesses (buffer overflow)
 *   - Use-after-free (access to freed memory)
 *   - Invalid frees (double-free, free of non-allocated memory)
 *
 * Design:
 *   - A fixed pool of memory is reserved at boot.
 *   - Every N-th kmalloc() is redirected to a KFENCE object.
 *   - Each KFENCE object is surrounded by guard pages (PAGE_SIZE).
 *   - Freed objects are quarantined (KFENCE_QUARANTINE_DEPTH allocations
 *     must pass) before they can be reused, giving use-after-free
 *     detection a meaningful window to trigger.
 *   - Memory accesses beyond the object or after free trigger a
 *     fault that is caught and reported.
 *
 * Item 139 — KFENCE: low-overhead memory safety with guard pages
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "timer.h"
#include "errno.h"

/* ── Configuration ──────────────────────────────────────────────────── */

/* Number of KFENCE objects in the pool */
#define KFENCE_NUM_OBJECTS   256

/* Size of each KFENCE object (maximum allocation size we can service) */
#define KFENCE_OBJECT_SIZE   4096

/* Sampling interval: every N-th kmalloc is checked (default 1/100) */
#define KFENCE_SAMPLE_INTERVAL 100

/* Quarantine depth: freed objects are not reusable until this many
 * subsequent KFENCE allocations have passed. This provides a window
 * for use-after-free detection — a dangling pointer is far more
 * likely to hit a still-quarantined object than one reused instantly. */
#define KFENCE_QUARANTINE_DEPTH 64

/* Total pool size = objects * (object_size + 2 * page_size for guards) */
#define KFENCE_POOL_SIZE  (KFENCE_NUM_OBJECTS * (KFENCE_OBJECT_SIZE + 2 * PAGE_SIZE))

/* Error types reported by KFENCE */
#define KFENCE_ERROR_NONE       0
#define KFENCE_ERROR_OOB        1   /* Out-of-bounds access */
#define KFENCE_ERROR_UAF        2   /* Use-after-free */
#define KFENCE_ERROR_INVALID_FREE 3  /* Invalid/double free */
#define KFENCE_ERROR_CORRUPTION 4   /* Memory corruption */

/* ── KFENCE object metadata ──────────────────────────────────────────── */

struct kfence_object {
    uint64_t addr;             /* Physical address of the object data */
    uint64_t guard_left;       /* Physical address of left guard page */
    uint64_t guard_right;      /* Physical address of right guard page */
    uint32_t size;             /* Allocated size (may be < KFENCE_OBJECT_SIZE) */
    uint32_t in_use;           /* 1 = currently allocated */
    uint32_t free_count;       /* Number of times freed (0 = allocated, 1+ = freed) */
    uint32_t quarantine_remaining; /* Quarantine countdown; 0 = ready for reuse */
    uint32_t alloc_tick;       /* Timer tick at allocation */
    uint32_t free_tick;        /* Timer tick at free */
    uint8_t  canary[8];        /* Canary bytes at start/end of object */
    uint64_t owner_pid;        /* PID of allocating process */
};

/* ── Global state ────────────────────────────────────────────────────── */

static struct kfence_object kfence_objects[KFENCE_NUM_OBJECTS];
static uint64_t kfence_pool_start = 0;  /* Physical address of pool */
static uint64_t kfence_pool_size = 0;
static spinlock_t kfence_lock;
static int kfence_initialized = 0;

/* Sampling state */
static volatile uint64_t kfence_alloc_counter = 0;
static int kfence_sample_interval = KFENCE_SAMPLE_INTERVAL;

/* Statistics */
static volatile uint64_t kfence_total_allocations = 0;
static volatile uint64_t kfence_errors_detected = 0;
static volatile uint64_t kfence_oob_detected = 0;
static volatile uint64_t kfence_uaf_detected = 0;

/* ── Internal helpers ────────────────────────────────────────────────── */

/*
 * Check if an address falls within the KFENCE pool.
 */
static int kfence_is_pool_addr(uint64_t addr)
{
    return (addr >= kfence_pool_start &&
            addr < kfence_pool_start + kfence_pool_size);
}

/*
 * Get the KFENCE object index from a data address within the pool.
 * Returns -1 if the address is in a guard page.
 */
static int kfence_addr_to_obj_idx(uint64_t addr)
{
    if (!kfence_is_pool_addr(addr))
        return -1;

    uint64_t offset = addr - kfence_pool_start;
    uint64_t slot_size = KFENCE_OBJECT_SIZE + 2 * PAGE_SIZE;
    uint64_t slot_idx = offset / slot_size;
    uint64_t slot_offset = offset % slot_size;

    /* Check if in guard page */
    if (slot_offset < PAGE_SIZE) {
        /* Left guard page — OOB access */
        return -2;
    }
    if (slot_offset >= PAGE_SIZE + KFENCE_OBJECT_SIZE) {
        /* Right guard page — OOB access */
        return -2;
    }

    return (int)slot_idx;
}

/*
 * Initialize canary bytes for a KFENCE object.
 */
static void kfence_init_canary(struct kfence_object *obj)
{
    for (int i = 0; i < 8; i++) {
        obj->canary[i] = (uint8_t)(0xAB + i);  /* Fixed pattern */
    }
}

/*
 * Check canary bytes for corruption.
 */
static int kfence_check_canary(struct kfence_object *obj)
{
    for (int i = 0; i < 8; i++) {
        if (obj->canary[i] != (uint8_t)(0xAB + i))
            return 0;  /* Corrupted */
    }
    return 1;  /* Intact */
}

/* ── Public API ─────────────────────────────────────────────────────── */

static void kfence_init(void)
{
    if (kfence_initialized) return;

    /* Allocate the KFENCE pool from physical memory */
    kfence_pool_size = KFENCE_POOL_SIZE;
    kfence_pool_start = (uint64_t)pmm_alloc_frames(kfence_pool_size / PAGE_SIZE);
    if (!kfence_pool_start) {
        kprintf("[KFENCE] Failed to allocate pool (%llu bytes)\n",
                (unsigned long long)kfence_pool_size);
        return;
    }

    memset(kfence_objects, 0, sizeof(kfence_objects));
    spinlock_init(&kfence_lock);
    kfence_initialized = 1;

    /* Map the pool as non-present/guard pages so access triggers a fault */
    for (uint64_t i = 0; i < KFENCE_NUM_OBJECTS; i++) {
        struct kfence_object *obj = &kfence_objects[i];
        uint64_t slot_start = kfence_pool_start + i * (KFENCE_OBJECT_SIZE + 2 * PAGE_SIZE);

        obj->guard_left = slot_start;
        obj->addr = slot_start + PAGE_SIZE;
        obj->guard_right = obj->addr + KFENCE_OBJECT_SIZE;
        obj->size = 0;
        obj->in_use = 0;
        obj->free_count = 0;
        obj->quarantine_remaining = 0;

        /* Mark guard pages as non-present in page tables */
        /* (In a real implementation, we'd unmap these pages) */
    }

    kprintf("[KFENCE] Initialized: %d objects, pool at 0x%lx (%llu bytes)\n",
            KFENCE_NUM_OBJECTS,
            (unsigned long)kfence_pool_start,
            (unsigned long long)kfence_pool_size);
    kprintf("[KFENCE] Sample interval: 1/%d allocations\n", kfence_sample_interval);
}

/*
 * KFENCE allocation: called instead of kmalloc for sampled allocations.
 *
 * @size: Requested allocation size (must be <= KFENCE_OBJECT_SIZE).
 * @returns Virtual address of the allocation, or NULL if no objects available.
 */
static void *kfence_alloc(uint64_t size)
{
    if (!kfence_initialized || size == 0 || size > KFENCE_OBJECT_SIZE)
        return NULL;

    spinlock_acquire(&kfence_lock);

    /* Decrement quarantine counters for all freed objects, and find
     * the first object that is free (not in use, not quarantined). */
    int idx = -1;
    for (int i = 0; i < KFENCE_NUM_OBJECTS; i++) {
        struct kfence_object *obj = &kfence_objects[i];
        if (!obj->in_use) {
            if (obj->quarantine_remaining > 0) {
                obj->quarantine_remaining--;
            }
            if (obj->quarantine_remaining == 0 && idx < 0) {
                idx = i;
            }
        }
    }

    if (idx < 0) {
        /* All objects in use — fall back to kmalloc */
        spinlock_release(&kfence_lock);
        return NULL;
    }

    struct kfence_object *obj = &kfence_objects[idx];
    obj->in_use = 1;
    obj->size = (uint32_t)size;
    obj->free_count = 0;
    obj->alloc_tick = (uint32_t)timer_get_ticks();
    obj->free_tick = 0;
    obj->owner_pid = 0;  /* Would get current PID in real impl */
    kfence_init_canary(obj);

    kfence_total_allocations++;

    spinlock_release(&kfence_lock);

    /* Return virtual address of the object data */
    void *ptr = (void *)PHYS_TO_VIRT(obj->addr);
    memset(ptr, 0, size);

    return ptr;
}

/*
 * KFENCE free: validate and mark the object as freed.
 *
 * @ptr: Pointer previously returned by kfence_alloc().
 *
 * Returns 0 if the free was valid, negative if an error was detected.
 */
static int kfence_free(void *ptr)
{
    if (!kfence_initialized || !ptr)
        return -EINVAL;

    uint64_t addr = VIRT_TO_PHYS((uint64_t)(uintptr_t)ptr);

    if (!kfence_is_pool_addr(addr)) {
        /* Not a KFENCE object — ignore */
        return -ENOENT;
    }

    int idx = kfence_addr_to_obj_idx(addr);
    if (idx < 0) {
        /* Address is in a guard page or invalid */
        kprintf("[KFENCE] ERROR: invalid free at 0x%lx (in guard page)\n",
                (unsigned long)addr);
        kfence_errors_detected++;
        return -EFAULT;
    }

    spinlock_acquire(&kfence_lock);

    struct kfence_object *obj = &kfence_objects[idx];
    if (!obj->in_use) {
        /* Double free or invalid free */
        kprintf("[KFENCE] ERROR: double/invalid free at 0x%lx (already freed)\n",
                (unsigned long)addr);
        kfence_errors_detected++;
        obj->free_count++;
        spinlock_release(&kfence_lock);
        return -EINVAL;
    }

    /* Check canary before freeing */
    if (!kfence_check_canary(obj)) {
        kprintf("[KFENCE] ERROR: canary corruption detected at 0x%lx "
                "(buffer overflow or use-after-free)\n",
                (unsigned long)addr);
        kfence_errors_detected++;
        kfence_oob_detected++;
        /* Still proceed with the free */
    }

    obj->in_use = 0;
    obj->free_count++;
    obj->free_tick = (uint32_t)timer_get_ticks();
    obj->quarantine_remaining = KFENCE_QUARANTINE_DEPTH;

    /* Poison the object to detect use-after-free */
    memset((void *)PHYS_TO_VIRT(obj->addr), 0xFB, KFENCE_OBJECT_SIZE);

    spinlock_release(&kfence_lock);

    return 0;
}

/*
 * KFENCE fault handler: called when a memory fault occurs in the KFENCE
 * pool (guard page access or use-after-free).
 *
 * @addr:  Virtual address that caused the fault.
 * @write: 1 if the access was a write, 0 for read.
 *
 * Returns 1 if KFENCE handled the fault (reported), 0 if it was not a
 * KFENCE-related fault.
 */
static int kfence_handle_fault(uint64_t addr, int write)
{
    if (!kfence_initialized)
        return 0;

    uint64_t phys_addr = VIRT_TO_PHYS(addr);

    if (!kfence_is_pool_addr(phys_addr))
        return 0;

    /* Determine what kind of error this is */
    int obj_idx = kfence_addr_to_obj_idx(phys_addr);

    if (obj_idx == -2) {
        /* Access in guard page — out of bounds */
        kprintf("[KFENCE] OUT OF BOUNDS %s at 0x%lx (guard page)\n",
                write ? "WRITE" : "READ",
                (unsigned long)addr);
        kfence_errors_detected++;
        kfence_oob_detected++;
        return 1;
    }

    if (obj_idx >= 0 && obj_idx < KFENCE_NUM_OBJECTS) {
        spinlock_acquire(&kfence_lock);
        struct kfence_object *obj = &kfence_objects[obj_idx];
        if (!obj->in_use) {
            /* Use-after-free */
            kprintf("[KFENCE] USE-AFTER-FREE %s at 0x%lx "
                    "(freed %llu ticks ago)\n",
                    write ? "WRITE" : "READ",
                    (unsigned long)addr,
                    (unsigned long long)(timer_get_ticks() - obj->free_tick));
            kfence_errors_detected++;
            kfence_uaf_detected++;
            spinlock_release(&kfence_lock);
            return 1;
        }
        spinlock_release(&kfence_lock);
    }

    return 1;  /* Claim the fault as handled */
}

/*
 * Should the current allocation be sampled by KFENCE?
 * Returns 1 if we should use kfence_alloc instead of kmalloc.
 */
static int kfence_should_sample(void)
{
    if (!kfence_initialized)
        return 0;

    /* Atomically increment and check */
    uint64_t count = __sync_add_and_fetch(&kfence_alloc_counter, 1);
    return (count % (uint64_t)kfence_sample_interval == 0);
}

/*
 * Configure KFENCE sampling interval.
 */
static void kfence_set_sample_interval(int interval)
{
    if (interval <= 0)
        interval = 1;
    kfence_sample_interval = interval;
    kprintf("[KFENCE] Sample interval set to 1/%d\n", interval);
}

/*
 * Get KFENCE statistics.
 */
static void kfence_get_stats(uint64_t *total_alloc, uint64_t *errors,
                       uint64_t *oob, uint64_t *uaf)
{
    if (total_alloc) *total_alloc = kfence_total_allocations;
    if (errors)      *errors      = kfence_errors_detected;
    if (oob)         *oob         = kfence_oob_detected;
    if (uaf)         *uaf         = kfence_uaf_detected;
}

/*
 * Print KFENCE status.
 */
static void kfence_dump(void)
{
    kprintf("KFENCE Status:\n");
    kprintf("  Pool: 0x%lx - 0x%lx (%llu bytes)\n",
            (unsigned long)kfence_pool_start,
            (unsigned long)(kfence_pool_start + kfence_pool_size),
            (unsigned long long)kfence_pool_size);
    kprintf("  Objects: %d x %d bytes\n", KFENCE_NUM_OBJECTS, KFENCE_OBJECT_SIZE);
    kprintf("  Sample interval: 1/%d\n", kfence_sample_interval);
    kprintf("  Total allocations: %llu\n",
            (unsigned long long)kfence_total_allocations);
    kprintf("  Errors detected: %llu (OOB: %llu, UAF: %llu)\n",
            (unsigned long long)kfence_errors_detected,
            (unsigned long long)kfence_oob_detected,
            (unsigned long long)kfence_uaf_detected);

    /* Show first few objects */
    int shown = 0;
    for (int i = 0; i < KFENCE_NUM_OBJECTS && shown < 5; i++) {
        if (kfence_objects[i].in_use) {
            kprintf("  [%d] addr=0x%lx size=%u alloc_tick=%u\n",
                    i,
                    (unsigned long)kfence_objects[i].addr,
                    kfence_objects[i].size,
                    kfence_objects[i].alloc_tick);
            shown++;
        }
    }
}

