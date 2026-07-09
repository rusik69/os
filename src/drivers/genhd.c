// SPDX-License-Identifier: GPL-2.0-only
/*
 * genhd.c — Generic disk layer
 *
 * Provides a standard abstraction for block device disks:
 *   - Allocation and deallocation (gendisk lifecycle)
 *   - Reference counting for safe sharing
 *   - Automatic /dev entry creation via devtmpfs
 *   - Driver-private data storage
 *   - Capacity management
 *
 * Each struct gendisk wraps a block device entry registered via the
 * blockdev_register() API.  Drivers that use the gendisk layer get
 * automatic device node creation and uniform naming.
 */

#include "genhd.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "types.h"
#include "blockdev.h"
#include "devtmpfs.h"
#include "export.h"

/* ── Global gendisk table ─────────────────────────────────────────── */

/* Static table of gendisk descriptors.  Each slot can be in use or free.
 * The struct itself is small (roughly 80 bytes), so we keep them in a
 * fixed-size array to avoid the allocator overhead for what is typically
 * a small number of disks (< 32). */
static struct gendisk g_disk_table[GENHD_MAX_DISKS];
static spinlock_t    g_table_lock;
static int           g_genhd_initialised;

/* ── Internal helpers ─────────────────────────────────────────────── */

/* Find a free slot in the gendisk table and claim it.
 * Returns the slot index, or -1 if the table is full. */
static int genhd_find_free_slot(void)
{
    for (int i = 0; i < GENHD_MAX_DISKS; i++) {
        if (!g_disk_table[i].in_use)
            return i;
    }
    return -1;
}

/* Look up a gendisk by its block device ID.
 * Returns NULL if no gendisk references this dev_id. */
static struct gendisk *genhd_lookup_by_dev_id(int dev_id)
{
    for (int i = 0; i < GENHD_MAX_DISKS; i++) {
        if (g_disk_table[i].in_use && g_disk_table[i].dev_id == dev_id)
            return &g_disk_table[i];
    }
    return NULL;
}

/* ── Initialisation ───────────────────────────────────────────────── */

void __init genhd_init(void)
{
    if (g_genhd_initialised)
        return;

    memset(g_disk_table, 0, sizeof(g_disk_table));
    spinlock_init(&g_table_lock);
    g_genhd_initialised = 1;

    kprintf("[OK] genhd: generic disk layer initialised (%d slots)\n",
            GENHD_MAX_DISKS);
}

/* ── Allocation ───────────────────────────────────────────────────── */

/**
 * alloc_disk - Allocate a gendisk structure
 * @minors:  Number of minor numbers to reserve (1 + max partitions)
 *
 * Returns a pointer to a zeroed gendisk on success, or NULL on failure.
 * The caller must fill in at least disk_name, capacity, and major/minor
 * before calling add_disk().
 *
 * The returned gendisk is NOT visible to the system until add_disk()
 * is called.  Its reference count starts at 1.
 */
struct gendisk *alloc_disk(int minors)
{
    struct gendisk *disk;
    int slot;

    if (!g_genhd_initialised)
        return NULL;

    if (minors < 1)
        minors = 1;

    slot = genhd_find_free_slot();
    if (slot < 0) {
        kprintf("[!!] genhd: no free slots (%d in use)\n", GENHD_MAX_DISKS);
        return NULL;
    }

    disk = &g_disk_table[slot];

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_table_lock, &irq_flags);

    memset(disk, 0, sizeof(*disk));
    disk->in_use    = 1;
    disk->minors    = minors;
    disk->ref       = 1;
    disk->sector_size = 512;  /* default sector size */

    spinlock_irqsave_release(&g_table_lock, irq_flags);

    return disk;
}
EXPORT_SYMBOL(alloc_disk);

/* ── Reference counting ───────────────────────────────────────────── */

/**
 * get_disk - Acquire a reference to a gendisk
 * @disk:  Gendisk to reference
 *
 * Returns the same pointer for convenience.  Must be paired with
 * put_disk().
 */
struct gendisk *get_disk(struct gendisk *disk)
{
    if (!disk || !disk->in_use)
        return NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_table_lock, &irq_flags);
    if (disk->in_use && disk->ref > 0)
        disk->ref++;
    spinlock_irqsave_release(&g_table_lock, irq_flags);

    return disk;
}
EXPORT_SYMBOL(get_disk);

/**
 * put_disk - Release a reference to a gendisk
 * @disk:  Gendisk to release (may be NULL)
 *
 * When the reference count drops to zero, the gendisk is freed
 * (marked as no longer in use).
 */
void put_disk(struct gendisk *disk)
{
    if (!disk)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_table_lock, &irq_flags);

    if (disk->ref > 0)
        disk->ref--;

    if (disk->ref == 0) {
        /* Free the slot */
        memset(disk, 0, sizeof(*disk));
    }

    spinlock_irqsave_release(&g_table_lock, irq_flags);
}
EXPORT_SYMBOL(put_disk);

/* ── Registration ─────────────────────────────────────────────────── */

/**
 * add_disk - Register a gendisk and make it visible
 * @disk:  Gendisk previously allocated with alloc_disk()
 *
 * Creates a /dev/<disk_name> entry via the device filesystem
 * (devtmpfs) so that userspace can open the block device.
 *
 * The caller is responsible for ensuring the underlying block
 * device is already registered (via blockdev_register) BEFORE
 * calling add_disk().  The gendisk layer does NOT register or
 * unregister the block device itself.
 *
 * Returns 0 on success, negative errno on error.
 */
int add_disk(struct gendisk *disk)
{
    int ret;

    if (!disk || !disk->in_use)
        return -EINVAL;

    if (!disk->disk_name[0]) {
        kprintf("[!!] genhd: add_disk called with empty name\n");
        return -EINVAL;
    }

    if (disk->capacity == 0) {
        kprintf("[!!] genhd: add_disk '%s' with zero capacity\n",
                disk->disk_name);
        return -EINVAL;
    }

    /* Sanity check: capacity must be a sane number of 512-byte sectors */
    if (disk->capacity > (1ULL << 60)) {
        kprintf("[!!] genhd: add_disk '%s' with implausible capacity %llu\n",
                disk->disk_name,
                (unsigned long long)disk->capacity);
        return -EINVAL;
    }

    /* Create the /dev/<name> block device node via devtmpfs.
     *
     * Major/minor numbers identify this specific disk to the VFS layer.
     * The convention used here is:
     *   - If the driver has set major/minor explicitly, use them.
     *   - Otherwise, auto-assign major=8 (sd-style) + slot offset. */
    int major = disk->major;
    int minor = disk->first_minor;

    if (major == 0 && minor == 0) {
        /* Auto-assign: use major 8 (common SCSI/SATA convention)
         * and minor = slot index * minors */
        major = 8;
        minor = 0;  /* first_minor will be set below */
    }

    /* Record the assigned numbers */
    disk->major = major;
    disk->first_minor = minor;

    /* Create the devtmpfs node */
    ret = devtmpfs_create_device(disk->disk_name, DT_BLOCK, (uint32_t)major, (uint32_t)minor);
    if (ret < 0) {
        kprintf("[!!] genhd: devtmpfs_create_device('%s') failed: %d\n",
                disk->disk_name, ret);
        return ret;
    }

    kprintf("[OK] genhd: disk '%s' added (major=%d minor=%d, %llu sectors, %d minors)\n",
            disk->disk_name, major, minor,
            (unsigned long long)disk->capacity, disk->minors);

    return 0;
}
EXPORT_SYMBOL(add_disk);

/**
 * del_gendisk - Unregister a gendisk
 * @disk:  Gendisk to remove
 *
 * Removes the /dev entry and marks the disk as no longer visible.
 * After this call, the disk is still accessible for in-flight I/O
 * completion via the reference count, but no new opens are possible.
 *
 * The caller must still call put_disk() to free the structure once
 * all references are gone.
 */
void del_gendisk(struct gendisk *disk)
{
    if (!disk || !disk->in_use)
        return;

    /* We cannot easily remove a devtmpfs node through the current API,
     * so we leave the /dev entry in place but mark it as inactive.
     * A future enhancement could add devtmpfs_remove_device().
     *
     * Clear the name so that lookups / iteration can detect
     * this disk as no longer valid. */
    disk->disk_name[0] = '\0';

    kprintf("[OK] genhd: disk removed\n");
}
EXPORT_SYMBOL(del_gendisk);

/* ── Table iteration ──────────────────────────────────────────────── */

/**
 * get_gendisk - Look up a gendisk by table index
 * @idx:  Slot index (0 .. GENHD_MAX_DISKS-1)
 *
 * Returns a gendisk pointer with an elevated refcount, or NULL if
 * the slot is empty.  Caller must call put_disk() when finished.
 */
struct gendisk *get_gendisk(int idx)
{
    if (idx < 0 || idx >= GENHD_MAX_DISKS || !g_genhd_initialised)
        return NULL;

    struct gendisk *disk = &g_disk_table[idx];

    if (!disk->in_use)
        return NULL;

    return get_disk(disk);
}

/* ── Lookup helpers (for filesystems / VFS integration) ───────────── */

/* Find a gendisk by its block device ID.
 * Returns a pointer with elevated refcount, or NULL. */
static struct gendisk *genhd_find_by_dev_id(int dev_id)
{
    for (int i = 0; i < GENHD_MAX_DISKS; i++) {
        if (g_disk_table[i].in_use && g_disk_table[i].dev_id == dev_id)
            return get_disk(&g_disk_table[i]);
    }
    return NULL;
}

/* Find a gendisk by its devtmpfs / VFS name.
 * Returns a pointer with elevated refcount, or NULL. */
static struct gendisk *genhd_find_by_name(const char *name)
{
    if (!name || !name[0])
        return NULL;

    for (int i = 0; i < GENHD_MAX_DISKS; i++) {
        if (g_disk_table[i].in_use &&
            g_disk_table[i].disk_name[0] &&
            strcmp(g_disk_table[i].disk_name, name) == 0)
            return get_disk(&g_disk_table[i]);
    }
    return NULL;
}

/* ── Module metadata (only when built as a loadable module) ──────── */
#ifdef MODULE
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("Generic disk layer (gendisk) — block device abstraction");
MODULE_AUTHOR("OS Kernel Team");
#endif
