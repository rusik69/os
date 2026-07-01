#ifndef GENHD_H
#define GENHD_H

#include "types.h"

/* Maximum number of gendisk slots */
#define GENHD_MAX_DISKS    32

/* Maximum disk name length (including NUL) */
#define GENHD_NAME_LEN     32

/* gendisk flags */
#define GENHD_FL_REMOVABLE    (1U << 0)  /* removable media (floppy, USB) */
#define GENHD_FL_NO_PART_SCAN (1U << 1)  /* skip partition scanning */

/* Forward declaration */
struct block_device_operations;

/**
 * struct gendisk - Generic disk descriptor
 *
 * Represents a single physical or virtual disk.  One gendisk corresponds
 * to one entry in the block device layer (blockdev) and may be subdivided
 * into partitions.
 */
struct gendisk {
    int            major;        /* device major number */
    int            first_minor;  /* first minor number */
    int            minors;       /* number of minors (partitions + 1) */
    char           disk_name[GENHD_NAME_LEN];  /* human-readable name, e.g. "sda" */

    int            dev_id;       /* blockdev entry id (index into g_blockdevs) */
    uint64_t       capacity;     /* size in sectors (512-byte units) */
    uint32_t       sector_size;  /* sector size in bytes (usually 512) */

    /* Reference count — 0 means freed */
    int            ref;

    /* Flags (GENHD_FL_*) */
    unsigned int   flags;

    /* Private data for the driver owning this disk */
    void          *private_data;

    /* Block device operations (open, release, ioctl, etc.) */
    const struct block_device_operations *fops;

    /* In-use flag for the static gendisk table */
    int            in_use;
};

/**
 * struct block_device_operations - Block device operation callbacks
 *
 * Called by the gendisk / blockdev layer when userspace or filesystem
 * code opens, releases, or performs ioctl on the device.
 */
struct block_device_operations {
    int (*open)(struct gendisk *disk, int mode);
    int (*release)(struct gendisk *disk, int mode);
    int (*ioctl)(struct gendisk *disk, unsigned int cmd, unsigned long arg);
};

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * alloc_disk - Allocate a gendisk structure
 * @minors:  Number of minor numbers to reserve (1 + max partitions)
 *
 * Returns a zeroed gendisk on success, NULL on OOM.
 * The disk is not yet visible — call add_disk() after setting fields.
 */
struct gendisk *alloc_disk(int minors);

/**
 * add_disk - Register a gendisk and make it visible to the system
 * @disk:  Gendisk previously allocated with alloc_disk()
 *
 * Registers the disk with the block device layer (if not already),
 * creates a /dev/<disk_name> entry via devtmpfs, and marks the disk
 * as ready for partition scanning.
 *
 * Returns 0 on success, negative errno on error.
 */
int add_disk(struct gendisk *disk);

/**
 * del_gendisk - Unregister a gendisk
 * @disk:  Gendisk to remove
 *
 * Removes the /dev entry, unregisters from the block device layer,
 * and marks the disk as no longer visible.  The caller must still
 * call put_disk() to free the structure.
 */
void del_gendisk(struct gendisk *disk);

/**
 * put_disk - Release a reference to a gendisk
 * @disk:  Gendisk to release (may be NULL)
 *
 * When the reference count reaches zero, the gendisk is freed.
 */
void put_disk(struct gendisk *disk);

/**
 * get_disk - Acquire a reference to a gendisk
 * @disk:  Gendisk to reference
 *
 * Returns the same pointer for convenience.
 */
struct gendisk *get_disk(struct gendisk *disk);

/**
 * set_capacity - Set the disk capacity
 * @disk:    Target gendisk
 * @sectors: New capacity in 512-byte sectors
 */
static inline void set_capacity(struct gendisk *disk, uint64_t sectors)
{
    if (disk)
        disk->capacity = sectors;
}

/**
 * get_capacity - Get the disk capacity in 512-byte sectors
 * @disk:  Target gendisk
 */
static inline uint64_t get_capacity(struct gendisk *disk)
{
    return disk ? disk->capacity : 0;
}

/**
 * disk_to_dev_id - Get the block device ID associated with a gendisk
 * @disk:  Target gendisk
 */
static inline int disk_to_dev_id(struct gendisk *disk)
{
    return disk ? disk->dev_id : -1;
}

/**
 * disk_to_name - Get the name of a gendisk
 * @disk:  Target gendisk
 */
static inline const char *disk_to_name(struct gendisk *disk)
{
    return disk ? disk->disk_name : "";
}

/* ── Table iteration (for partition scanning / debug) ────────────── */

/**
 * get_gendisk - Look up a gendisk by index in the global table
 * @idx:  Index (0 .. GENHD_MAX_DISKS-1)
 *
 * Returns a pointer (with elevated refcount) or NULL if the slot is
 * empty.  Caller must put_disk() when done.
 */
struct gendisk *get_gendisk(int idx);

/**
 * genhd_init - Initialise the gendisk subsystem
 *
 * Called once during kernel boot.
 */
void genhd_init(void);

#endif /* GENHD_H */
