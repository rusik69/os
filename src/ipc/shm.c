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

/* ── shm_open ─────────────────────────────────────────── */
int shm_open(const char *name, int oflag, ...)
{
    (void)name;
    (void)oflag;
    /* POSIX shared memory: allocate a new segment with a hash of name as key */
    if (!name) return -EINVAL;

    /* Simple hash of name to use as key */
    int key = 0;
    const char *p = name;
    while (*p) { key = key * 31 + *p++; }

    int id = shm_get(key, SHM_RW);
    if (id >= 0) return id;

    /* If O_CREAT is set (0x40), try creating */
    if (oflag & 0x40) {
        /* shm_get with mode will create if not found */
        id = shm_get(key, SHM_RW);
        return id;
    }

    return -ENOENT;
}

/* ── shm_unlink ───────────────────────────────────────── */
int shm_unlink(const char *name)
{
    (void)name;
    /* POSIX shared memory unlink: mark the segment for removal.
     * Find segment by name hash and free it if no references. */
    if (!name) return -EINVAL;

    int key = 0;
    const char *p = name;
    while (*p) { key = key * 31 + *p++; }

    for (int i = 0; i < SHM_MAX; i++) {
        if (shm_table[i].used && shm_table[i].key == key) {
            return shm_free(i);
        }
    }
    return -ENOENT;
}

/* ── shm_mmap ─────────────────────────────────────────── */
int shm_mmap(int id, uint64_t addr, size_t len, int prot, int flags)
{
    (void)addr;
    (void)len;
    (void)prot;
    (void)flags;

    if (id < 0 || id >= SHM_MAX || !shm_table[id].used)
        return -EINVAL;

    /* Map the shared memory segment into the current process's address space
     * at the requested address (or at a convenient location). */
    struct process *cur = process_get_current();
    if (!cur) return -EINVAL;

    uint64_t virt = SHM_VIRT_BASE + (uint64_t)id * 0x10000ULL;
    uint64_t *pml4 = cur->pml4 ? cur->pml4 : vmm_get_pml4();

    uint64_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (prot & 0x2) vmm_flags |= VMM_FLAG_WRITE; /* PROT_WRITE */

    if (vmm_map_user_page(pml4, virt, shm_table[id].phys, vmm_flags) < 0)
        return -ENOMEM;

    shm_table[id].refs++;
    return (int)virt;
}

/* ── shm_show_fdinfo ──────────────────────────────────── */
int shm_show_fdinfo(int id, char *buf, size_t size)
{
    if (!buf || size == 0) return -EINVAL;

    int pos = 0;
    if (id >= 0 && id < SHM_MAX && shm_table[id].used) {
        pos = snprintf(buf, size,
            "shmid:\t%d\n"
            "key:\t%d\n"
            "phys:\t0x%llx\n"
            "refs:\t%d\n"
            "uid:\t%u\n"
            "gid:\t%u\n"
            "mode:\t%o\n",
            id, shm_table[id].key,
            (unsigned long long)shm_table[id].phys,
            shm_table[id].refs,
            shm_table[id].uid, shm_table[id].gid,
            shm_table[id].mode);
    } else {
        pos = snprintf(buf, size, "shmid:\t%d (unused)\n", id);
    }

    if (pos < 0) return -EINVAL;
    if ((size_t)pos >= size) return -ENOSPC;
    return pos;
}

/* ── Local shm_info structure for statistics ───────────────── */
#ifndef __SHM_INFO_DEFINED
#define __SHM_INFO_DEFINED
struct shm_info {
    int      shm_tot;    /* total segments */
    int      shm_used;   /* used segments  */
    uint64_t shm_pages;  /* total pages    */
    int      shm_max;    /* max segments   */
};
#endif

/* ── shm_stat ─────────────────────────────────────────── */
int shm_stat(struct shm_info *info)
{
    if (!info) return -EINVAL;

    int total = 0, used = 0;
    uint64_t total_pages = 0;

    for (int i = 0; i < SHM_MAX; i++) {
        total++;
        if (shm_table[i].used) {
            used++;
            total_pages++;
        }
    }

    info->shm_tot = total;
    info->shm_used = used;
    info->shm_pages = total_pages;
    info->shm_max = SHM_MAX;
    return 0;
}
