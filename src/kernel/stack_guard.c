#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "stack_guard.h"
#include "vmm.h"
#include "pmm.h"
#include "errno.h"

/* Size of guard region tracking table */
#define STACK_GUARD_MAX_REGIONS 64

struct guard_region {
    uint64_t stack_base;   /* virtual address of the lowest stack page */
    uint64_t guard_addr;   /* virtual address of the guard page */
    uint64_t guard_phys;   /* physical address of guard page (0 = none allocated) */
    int      pages;        /* number of stack pages */
    int      used;
};

static struct guard_region guard_regions[STACK_GUARD_MAX_REGIONS];
static int guard_initialized = 0;

void stack_guard_init(void)
{
    memset(guard_regions, 0, sizeof(guard_regions));
    guard_initialized = 1;
    kprintf("[OK] stack_guard: kernel stack guard page tracking initialized\n");
}

int stack_guard_setup(uint64_t stack_virt, int stack_pages)
{
    if (!guard_initialized)
        return -ENODEV;

    /* Find a free tracking slot */
    int slot = -1;
    for (int i = 0; i < STACK_GUARD_MAX_REGIONS; i++) {
        if (!guard_regions[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -ENOMEM;

    /* The guard page is the page just below the stack.
     * stack_virt is the stack top (highest address).
     * The stack occupies pages from stack_virt - stack_pages*PAGE_SIZE to stack_virt.
     * The guard page is at stack_virt - (stack_pages + 1) * PAGE_SIZE. */
    uint64_t guard_addr = stack_virt - (uint64_t)(stack_pages + 1) * PAGE_SIZE;
    uint64_t stack_base = stack_virt - (uint64_t)stack_pages * PAGE_SIZE;

    /* Unmap the guard page if it was mapped. We allocate a physical frame
     * but keep it unmapped so any access triggers a page fault.
     * The physical address is stored so it can be freed on removal. */
    uint64_t existing = vmm_get_physaddr(guard_addr);
    if (existing != 0) {
        vmm_unmap_page(guard_addr);
    }

    /* Allocate a physical frame to reserve the backing memory for the guard.
     * Even though the page is never mapped, we keep the physical page reserved
     * so that no other allocator can claim it.  It is freed in
     * stack_guard_remove() to prevent memory leaks. */
    uint64_t guard_phys = pmm_alloc_frame();
    if (guard_phys == 0) {
        return -ENOMEM;
    }
    /* Do NOT map it — that's the whole point of a guard page. */

    guard_regions[slot].stack_base  = stack_base;
    guard_regions[slot].guard_addr  = guard_addr;
    guard_regions[slot].guard_phys  = guard_phys;
    guard_regions[slot].pages       = stack_pages;
    guard_regions[slot].used        = 1;

    return 0;
}

int stack_guard_check(uint64_t fault_addr)
{
    if (!guard_initialized)
        return 0;

    for (int i = 0; i < STACK_GUARD_MAX_REGIONS; i++) {
        if (guard_regions[i].used &&
            fault_addr >= guard_regions[i].guard_addr &&
            fault_addr < guard_regions[i].guard_addr + PAGE_SIZE) {
            return 1; /* guard page violation detected */
        }
    }
    return 0;
}

int stack_guard_remove(uint64_t stack_virt, int stack_pages)
{
    if (!guard_initialized)
        return -ENODEV;

    uint64_t guard_addr = stack_virt - (uint64_t)(stack_pages + 1) * PAGE_SIZE;

    for (int i = 0; i < STACK_GUARD_MAX_REGIONS; i++) {
        if (guard_regions[i].used && guard_regions[i].guard_addr == guard_addr) {
            /* Free the physical frame that was allocated for this guard page */
            if (guard_regions[i].guard_phys != 0) {
                pmm_free_frame(guard_regions[i].guard_phys);
                guard_regions[i].guard_phys = 0;
            }
            guard_regions[i].used = 0;
            return 0;
        }
    }
    return -ENOENT;
}

/* ── Stub: stack_guard_handle_violation ─────────────────────────────── */
int stack_guard_handle_violation(void *task)
{
    (void)task;
    kprintf("[stack] stack_guard_handle_violation: not yet implemented\n");
    return -ENOSYS;
}
