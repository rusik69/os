/*
 * shm.c — shared memory segments
 *
 * 16 segments, each one 4096-byte page.
 * shmget(key) → segment id (0-15), or -1 if no slots.
 * shmat(id)   → maps the segment page into the caller's PML4; returns virt addr or 0.
 * shmdt(id)   → unmaps from caller's PML4.
 * shmfree(id) → free the segment (last user responsibility).
 */

#include "shm.h"
#include "pmm.h"
#include "vmm.h"
#include "process.h"
#include "string.h"
#include "printf.h"

#define SHM_MAX      16
#define SHM_PAGE     4096
#define SHM_VIRT_BASE 0x0000700000000000ULL  /* user-space mapping base */

struct shm_slot {
    int      used;
    int      key;
    uint64_t phys;   /* physical frame address */
    int      refs;
};

static struct shm_slot shm_table[SHM_MAX];

void shm_init(void) {
    for (int i = 0; i < SHM_MAX; i++) {
        shm_table[i].used = 0;
        shm_table[i].phys = 0;
        shm_table[i].refs = 0;
    }
}

int shm_get(int key) {
    /* Look for existing segment with this key */
    for (int i = 0; i < SHM_MAX; i++) {
        if (shm_table[i].used && shm_table[i].key == key) return i;
    }
    /* Allocate new */
    for (int i = 0; i < SHM_MAX; i++) {
        if (!shm_table[i].used) {
            uint64_t frame = pmm_alloc_frame();
            if (!frame) return -1;
            /* Zero the shared page */
            memset((void *)frame, 0, SHM_PAGE);
            shm_table[i].used = 1;
            shm_table[i].key  = key;
            shm_table[i].phys = frame;
            shm_table[i].refs = 0;
            return i;
        }
    }
    return -1; /* no slots */
}

/* Map segment id into current process's address space.
 * Returns the virtual address, or 0 on failure. */
uint64_t shm_at(int id) {
    if (id < 0 || id >= SHM_MAX || !shm_table[id].used) return 0;
    struct process *cur = process_get_current();
    if (!cur) return 0;

    /* Each segment is mapped at SHM_VIRT_BASE + id * 0x10000 in user space */
    uint64_t virt = SHM_VIRT_BASE + (uint64_t)id * 0x10000ULL;

    /* Map into current process PML4 (kernel PML4 if kernel process) */
    uint64_t *pml4 = cur->pml4 ? cur->pml4 : vmm_get_pml4();
    if (vmm_map_user_page(pml4, virt, shm_table[id].phys,
                          VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER) < 0)
        return 0;
    shm_table[id].refs++;
    return virt;
}

/* Unmap segment from current process */
int shm_dt(int id) {
    if (id < 0 || id >= SHM_MAX || !shm_table[id].used) return -1;
    if (shm_table[id].refs > 0) {
        shm_table[id].refs--;
        /* Unmap from current process's page table */
        struct process *cur = process_get_current();
        if (cur) {
            uint64_t virt = SHM_VIRT_BASE + (uint64_t)id * 0x10000ULL;
            uint64_t *pml4 = cur->pml4 ? cur->pml4 : vmm_get_pml4();
            vmm_unmap_user_page(pml4, virt);
        }
    }
    return 0;
}

/* Free the segment (releases physical frame) */
int shm_free(int id) {
    if (id < 0 || id >= SHM_MAX || !shm_table[id].used) return -1;
    if (shm_table[id].refs > 0) return -1; /* still mapped by one or more processes */
    pmm_free_frame(shm_table[id].phys);
    shm_table[id].used = 0;
    shm_table[id].refs = 0;
    shm_table[id].phys = 0;
    return 0;
}
