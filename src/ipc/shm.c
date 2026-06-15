/*
 * shm.c — shared memory segments with permission control
 *
 * 16 segments, each one 4096-byte page.
 * shmget(key, mode) → segment id (0-15), or -1 if no slots.
 *   - If segment exists: returns id, mode is ignored.
 *   - If new: owner = current process uid/gid, mode stored.
 * shmat(id)   → maps the segment page into the caller's PML4;
 *               checks read permission. Returns virt addr or 0.
 * shmdt(id)   → unmaps from caller's PML4 (always allowed).
 * shmfree(id) → free the segment; owner or root only.
 */

#include "shm.h"
#include "pmm.h"
#include "vmm.h"
#include "process.h"
#include "string.h"
#include "printf.h"
#include "users.h"

#define SHM_MAX      16
#define SHM_PAGE     4096
#define SHM_VIRT_BASE  0x0000700000000000ULL  /* user-space mapping base */

/* ── Slot structure (was shm_slot, now carries ownership) ─────────── */

struct shm_slot {
    int      used;
    int      key;
    uint64_t phys;   /* physical frame address */
    int      refs;
    uint32_t uid;    /* owner user ID */
    uint32_t gid;    /* owner group ID */
    uint16_t mode;   /* permission bits (rwxrwxrwx) */
};

/* ── Permission helpers ───────────────────────────────────────────── */

/* Check if the current process has the requested access mode bits.
 * Follows Unix semantics: owner → owner bits, group → group bits, else other bits.
 * Returns 1 if allowed, 0 if denied. */
static int shm_check_perm(struct shm_slot *slot, uint16_t access_needed)
{
    struct process *cur = process_get_current();
    if (!cur) return 0;               /* kernel context? deny */
    if (cur->uid == 0) return 1;      /* root can do anything */

    uint16_t mode = slot->mode;
    uint16_t granted = 0;

    if (cur->uid == slot->uid) {
        /* Owner: check owner bits */
        granted = (mode >> 6) & 7;           /* owner bits */
    } else if (cur->gid == slot->gid ||
               user_in_group(cur->uid, slot->gid)) {
        /* Group member: check group bits */
        granted = (mode >> 3) & 7;           /* group bits */
    } else {
        /* Everyone else: check other bits */
        granted = mode & 7;                   /* other bits */
    }

    return (granted & access_needed) == access_needed;
}

static struct shm_slot shm_table[SHM_MAX];

void shm_init(void)
{
    for (int i = 0; i < SHM_MAX; i++) {
        shm_table[i].used = 0;
        shm_table[i].phys = 0;
        shm_table[i].refs = 0;
        shm_table[i].uid  = 0;
        shm_table[i].gid  = 0;
        shm_table[i].mode = SHM_RW;  /* default: owner rw */
    }
}

int shm_get(int key, uint16_t mode)
{
    /* Look for existing segment with this key */
    for (int i = 0; i < SHM_MAX; i++) {
        if (shm_table[i].used && shm_table[i].key == key) {
            /* Existing segment — check that caller can access it.
             * For shmget of an existing segment, we need at least
             * SHM_R permission on the segment (POSIX semantics). */
            if (!shm_check_perm(&shm_table[i], SHM_R >> 6)) {
                kprintf("shm: permission denied for key %d (id %d)\n", key, i);
                return -EACCES;
            }
            return i;
        }
    }

    /* Allocate new segment */
    for (int i = 0; i < SHM_MAX; i++) {
        if (!shm_table[i].used) {
            uint64_t frame = pmm_alloc_frame();
            if (!frame) return -ENOMEM;

            /* Pin the shared page */
            pmm_ref_frame(frame);

            /* Zero the shared page */
            memset(PHYS_TO_VIRT(frame), 0, SHM_PAGE);

            /* Set ownership and permissions */
            struct process *cur = process_get_current();
            uint32_t uid = cur ? cur->uid : 0;
            uint32_t gid = cur ? cur->gid : 0;

            /* Default to SHM_RW if mode is 0 (backward compat) */
            if (mode == 0) mode = SHM_RW;

            shm_table[i].used = 1;
            shm_table[i].key  = key;
            shm_table[i].phys = frame;
            shm_table[i].refs = 0;
            shm_table[i].uid  = uid;
            shm_table[i].gid  = gid;
            shm_table[i].mode = mode & SHM_MODE_MASK;

            return i;
        }
    }
    return -ENOSPC; /* no slots */
}

uint64_t shm_at(int id)
{
    if (id < 0 || id >= SHM_MAX || !shm_table[id].used) return 0;

    /* Permission check: caller needs at least read+write on the segment
     * to map it (you can't map a segment you can't read). */
    if (!shm_check_perm(&shm_table[id], SHM_R >> 6)) {
        kprintf("shm: attach denied for segment %d\n", id);
        return 0;
    }

    struct process *cur = process_get_current();
    if (!cur) return 0;

    uint64_t virt = SHM_VIRT_BASE + (uint64_t)id * 0x10000ULL;

    uint64_t *pml4 = cur->pml4 ? cur->pml4 : vmm_get_pml4();
    if (vmm_map_user_page(pml4, virt, shm_table[id].phys,
                          VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER) < 0)
        return 0;
    shm_table[id].refs++;
    return virt;
}

int shm_dt(int id)
{
    if (id < 0 || id >= SHM_MAX || !shm_table[id].used) return -EINVAL;
    if (shm_table[id].refs > 0) {
        shm_table[id].refs--;
        struct process *cur = process_get_current();
        if (cur) {
            uint64_t virt = SHM_VIRT_BASE + (uint64_t)id * 0x10000ULL;
            uint64_t *pml4 = cur->pml4 ? cur->pml4 : vmm_get_pml4();
            vmm_unmap_user_page(pml4, virt);
        }
    }
    return 0;
}

int shm_free(int id)
{
    if (id < 0 || id >= SHM_MAX || !shm_table[id].used) return -EINVAL;

    /* Permission check: only owner or root can free */
    struct process *cur = process_get_current();
    uint32_t caller_uid = cur ? cur->uid : 0;
    if (caller_uid != 0 && caller_uid != shm_table[id].uid) {
        kprintf("shm: free denied for segment %d (not owner)\n", id);
        return -EACCES;
    }

    if (shm_table[id].refs > 0) return -EBUSY; /* still mapped */

    pmm_unref_frame(shm_table[id].phys);
    shm_table[id].used = 0;
    shm_table[id].refs = 0;
    shm_table[id].phys = 0;
    shm_table[id].uid  = 0;
    shm_table[id].gid  = 0;
    shm_table[id].mode = SHM_RW;
    return 0;
}

int shm_perm_set(int id, uint32_t uid, uint32_t gid, uint16_t mode)
{
    if (id < 0 || id >= SHM_MAX || !shm_table[id].used) return -EINVAL;

    struct process *cur = process_get_current();
    uint32_t caller_uid = cur ? cur->uid : 0;

    /* Only owner or root can change permissions */
    if (caller_uid != 0 && caller_uid != shm_table[id].uid) {
        kprintf("shm: perm_set denied for segment %d (not owner)\n", id);
        return -EACCES;
    }

    /* Only root can change ownership */
    if (uid != (uint32_t)-1 && caller_uid != 0) {
        if (uid != shm_table[id].uid) {
            kprintf("shm: only root can change ownership\n");
            return -EACCES;
        }
    }

    if (uid != (uint32_t)-1) shm_table[id].uid = uid;
    if (gid != (uint32_t)-1) shm_table[id].gid = gid;
    if (mode != (uint16_t)-1) shm_table[id].mode = mode & SHM_MODE_MASK;

    return 0;
}

int shm_perm_get(int id, struct shm_perm *out)
{
    if (id < 0 || id >= SHM_MAX) return -EINVAL;
    if (!out) return -EINVAL;

    out->id   = id;
    out->key  = shm_table[id].used ? shm_table[id].key : 0;
    out->uid  = shm_table[id].uid;
    out->gid  = shm_table[id].gid;
    out->mode = shm_table[id].mode;
    out->refs = shm_table[id].refs;
    out->phys = shm_table[id].phys;
    out->used = shm_table[id].used;

    return 0;
}
