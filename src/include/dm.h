#ifndef DM_H
#define DM_H

#include "types.h"
#include "list.h"
#include "blockdev.h"

/* ── Device mapper constants ──────────────────────────────────────── */

#define DM_MAX_DEVICES      8       /* max virtual devices */
#define DM_MAX_TARGETS      16      /* max targets per device */
#define DM_NAME_MAX         32      /* max device name length */
#define DM_TARGET_NAME_MAX  16      /* max target type name length */

/* Device mapper device numbers (use a reserved blockdev range) */
#define DM_DEVBASE          24      /* first dm blockdev ID */
#define DM_DEV_COUNT        8       /* number of dm blockdev IDs */

/* IOCTL-like commands (passed via a control interface) */
#define DM_IOCTL_CREATE     1       /* create a new dm device */
#define DM_IOCTL_REMOVE     2       /* remove a dm device */
#define DM_IOCTL_SUSPEND    3       /* suspend I/O on device */
#define DM_IOCTL_RESUME     4       /* resume I/O on device */
#define DM_IOCTL_TABLE_LOAD 5       /* load a mapping table */
#define DM_IOCTL_TABLE_CLEAR 6      /* clear the mapping table */
#define DM_IOCTL_STATUS     7       /* query status */

/* Target flags */
#define DM_TARGET_PASSES_THROUGH 1  /* device doesn't modify data (linear) */
#define DM_TARGET_ENCRYPTS       2  /* encrypts/decrypts data (crypt) */

/* ── Forward declarations ─────────────────────────────────────────── */

struct dm_device;
struct dm_target;

/* ── Target operations — each target type registers these ─────────── */

struct dm_target_ops {
    /* Target type name (e.g., "linear", "crypt", "raid0") */
    const char *name;

    /* Flags (DM_TARGET_PASSES_THROUGH, etc.) */
    uint32_t flags;

    /* Construct a target instance from a table line.
     * @param ti   target instance to initialise
     * @param argc number of arguments
     * @param argv argument strings (parsed from table load)
     * Returns 0 on success, negative errno on failure. */
    int (*ctr)(struct dm_target *ti, int argc, const char **argv);

    /* Destroy a target instance (free allocated resources). */
    void (*dtr)(struct dm_target *ti);

    /* Map a bio/request — redirect to the backing device(s).
     * @param ti     target instance
     * @param req    the incoming block request
     * @param mapped output: one or more remapped requests to submit
     * @param mapped_count output: number of mapped requests
     * Returns 0 on success, negative errno on failure.
     * The target is responsible for cloning the request if needed. */
    int (*map)(struct dm_target *ti, struct blk_request *req,
               struct blk_request *mapped[], int *mapped_count);
};

/* ── Target instance ──────────────────────────────────────────────── */

struct dm_target {
    /* Owning device mapper device */
    struct dm_device *dm;

    /* Target type operations */
    const struct dm_target_ops *ops;

    /* Sector range this target covers (on the virtual device) */
    uint64_t start;      /* first virtual sector (inclusive) */
    uint64_t length;     /* number of sectors this target covers */

    /* Private data for this target instance (type-specific) */
    void *private;

    /* Linked list entry in dm_device->targets */
    struct list_head list;
};

/* ── Device Mapper device (virtual block device) ──────────────────── */

struct dm_device {
    /* Device name (e.g., "myvol", "cryptroot") */
    char name[DM_NAME_MAX];

    /* Block device ID (from the reserved DM_DEVBASE range) */
    int dev_id;

    /* State flags */
    int active;          /* 1 if device exists */
    int suspended;       /* 1 if I/O is suspended */

    /* Total size in sectors */
    uint64_t sector_count;

    /* List of targets (ordered by start sector) */
    struct list_head targets;
    int num_targets;

    /* Lock for this device */
    spinlock_t lock;

    /* Pending request queue (accumulated while suspended) */
    struct blk_request *suspended_head;
    struct blk_request *suspended_tail;
    int suspended_count;
};

/* ── Target type registration ─────────────────────────────────────── */

#define DM_MAX_TARGET_TYPES 16

struct dm_target_type {
    char name[DM_TARGET_NAME_MAX];
    const struct dm_target_ops *ops;
    int registered;
};

/* ── Public API ───────────────────────────────────────────────────── */

/* Initialise the device mapper subsystem. */
void dm_init(void);

/* Register the dm-linear target type. */
void dm_linear_init(void);

/* Register the dm-zero test target (reads as zeros, discards writes). */
void dm_zero_init(void);

/* Register the dm-error test target (all I/O fails with -EIO). */
void dm_error_init(void);

/* Register the dm-crypt transparent encryption target. */
void dm_crypt_init(void);

/* Register the dm-verity integrity target (Merkle hash tree verification). */
void dm_verity_init(void);

/* Register a target type (called by target modules). */
int dm_register_target(const struct dm_target_ops *ops);

/* Unregister a target type. */
int dm_unregister_target(const struct dm_target_ops *ops);

/* Find a target type by name. */
const struct dm_target_ops *dm_find_target(const char *name);

/* Create a device mapper device.
 * @param name  device name (e.g., "myvol")
 * @param size  size in sectors
 * Returns dm device ID (>= 0) on success, negative errno on failure. */
int dm_device_create(const char *name, uint64_t size);

/* Remove a device mapper device.
 * Returns 0 on success, negative errno if busy or not found. */
int dm_device_remove(int dm_id);

/* Suspend a device mapper device — queue incoming I/O.
 * Returns 0 on success. */
int dm_device_suspend(int dm_id);

/* Resume a device mapper device — flush queued I/O.
 * Returns 0 on success. */
int dm_device_resume(int dm_id);

/* Load a mapping table — replaces any existing table.
 * Format: "start_sectors length target_type args..."
 * Lines separated by newlines ('\n').
 * Returns 0 on success, negative errno on failure. */
int dm_table_load(int dm_id, const char *table);

/* Clear the mapping table for a device. */
int dm_table_clear(int dm_id);

/* Get the number of targets in the current table. */
int dm_table_target_count(int dm_id);

/* Query device status — fill a buffer with human-readable status.
 * Returns bytes written, or negative errno. */
int dm_device_status(int dm_id, char *buf, int max);

/* Find a dm_device by ID. Returns NULL if not active. */
struct dm_device *dm_device_get(int dm_id);

/* Look up a dm_device by name. Returns NULL if not found. */
struct dm_device *dm_device_find(const char *name);

/* -- for use by blockdev submit_fn -- */
int dm_submit_request(struct blk_request *req);

#endif /* DM_H */
