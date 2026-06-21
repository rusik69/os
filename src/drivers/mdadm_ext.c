#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "mdadm.h"
#include "string.h"
#include "blockdev.h"
#include "spinlock.h"
#include "export.h"
#include "heap.h"
#include "errno.h"

/* ── Maximum number of RAID arrays ─────────────────────────────────── */
#define MAX_RAID1_ARRAYS 4
#define MAX_RAID0_ARRAYS 4
#define MAX_RAID10_ARRAYS 4
#define MAX_RAID5_ARRAYS 4
#define MAX_RAID6_ARRAYS 4

/* Global tables for each RAID level */
static struct raid1_array  g_raid1_arrays[MAX_RAID1_ARRAYS];
static struct raid0_array  g_raid0_arrays[MAX_RAID0_ARRAYS];
static struct raid10_array g_raid10_arrays[MAX_RAID10_ARRAYS];
static struct raid5_array  g_raid5_arrays[MAX_RAID5_ARRAYS];
static struct raid6_array  g_raid6_arrays[MAX_RAID6_ARRAYS];
static spinlock_t g_raid_lock;
static int g_raid_next_id = 0; /* next MD block device id offset from MD_BLOCKDEV_BASE */
static int g_raid_initialized = 0;

/* ── Forward declarations ──────────────────────────────────────────── */
static int raid1_submit_fn(struct blk_request *req);
static int raid0_submit_fn(struct blk_request *req);
static int raid10_submit_fn(struct blk_request *req);
static int raid5_submit_fn(struct blk_request *req);
static int raid6_submit_fn(struct blk_request *req);

/* ── Initialization ────────────────────────────────────────────────── */

void raid_md_init(void)
{
    if (g_raid_initialized) return;
    memset(g_raid1_arrays, 0, sizeof(g_raid1_arrays));
    memset(g_raid0_arrays, 0, sizeof(g_raid0_arrays));
    memset(g_raid10_arrays, 0, sizeof(g_raid10_arrays));
    memset(g_raid5_arrays, 0, sizeof(g_raid5_arrays));
    memset(g_raid6_arrays, 0, sizeof(g_raid6_arrays));
    spinlock_init(&g_raid_lock);
    g_raid_next_id = 0;
    g_raid_initialized = 1;
    kprintf("[mdadm] MD subsystem initialized (RAID0/1/5/6/10, max %d+%d+%d+%d+%d arrays)\n",
            MAX_RAID1_ARRAYS, MAX_RAID0_ARRAYS, MAX_RAID5_ARRAYS,
            MAX_RAID6_ARRAYS, MAX_RAID10_ARRAYS);
}

/* ── Synchronous I/O helper on a member device ─────────────────────── */

/*
 * Perform synchronous sector I/O on a member block device.
 * Returns 0 on success, -errno on error.
 */
static int member_io(int dev_id, uint64_t lba, uint32_t count,
                     void *buf, int is_write)
{
    uint32_t flags = is_write ? BLK_REQ_WRITE : BLK_REQ_READ;
    return blk_submit_sync(dev_id, lba, count, buf, flags);
}

/* ── Scan available member devices for a RAID1 array ───────────────── */

/*
 * Find the effective sector count for a RAID1 array:
 * the minimum sector count across all active members.
 */
static uint64_t raid1_min_sectors(const struct raid1_array *array)
{
    uint64_t min_sectors = (uint64_t)-1;
    for (int i = 0; i < array->num_members; i++) {
        if (array->members[i].state == RAID_MEMBER_ACTIVE) {
            uint64_t ns = blockdev_get_sectors(array->members[i].dev_id);
            if (ns < min_sectors)
                min_sectors = ns;
        }
    }
    if (min_sectors == (uint64_t)-1)
        return 0;
    return min_sectors;
}

/* ── Create a RAID1 array ──────────────────────────────────────────── */

int raid1_create(const int *member_dev_ids, int num_members)
{
    if (!member_dev_ids || num_members < 2 || num_members > RAID1_MAX_MEMBERS) {
        kprintf("[mdadm] RAID1: invalid parameters (%d members, max %d)\n",
                num_members, RAID1_MAX_MEMBERS);
        return -1;
    }

    if (!g_raid_initialized)
        raid_md_init();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_RAID1_ARRAYS; i++) {
        if (g_raid1_arrays[i].num_members == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        kprintf("[mdadm] RAID1: no free array slots\n");
        return -1;
    }

    /* Compute the MD block device id */
    int md_id = MD_BLOCKDEV_BASE + g_raid_next_id;
    g_raid_next_id = (g_raid_next_id + 1) % (BLOCKDEV_MAX_DEVICES - MD_BLOCKDEV_BASE);

    /* Build the member list */
    memset(&g_raid1_arrays[slot], 0, sizeof(struct raid1_array));
    g_raid1_arrays[slot].array_id = md_id;
    g_raid1_arrays[slot].state = RAID_ARRAY_CLEAN;
    g_raid1_arrays[slot].num_members = num_members;
    g_raid1_arrays[slot].array_sectors = 0;

    for (int i = 0; i < num_members; i++) {
        int dev_id = member_dev_ids[i];
        g_raid1_arrays[slot].members[i].dev_id = dev_id;
        g_raid1_arrays[slot].members[i].state = RAID_MEMBER_ACTIVE;
        g_raid1_arrays[slot].members[i].sector_count = blockdev_get_sectors(dev_id);

        if (g_raid1_arrays[slot].members[i].sector_count == 0) {
            kprintf("[mdadm] RAID1: member %d (dev_id=%d) has 0 sectors\n",
                    i, dev_id);
            spinlock_irqsave_release(&g_raid_lock, irq_flags);
            memset(&g_raid1_arrays[slot], 0, sizeof(struct raid1_array));
            return -1;
        }
    }

    /* Minimum sector count across all members defines the array size */
    g_raid1_arrays[slot].array_sectors = raid1_min_sectors(&g_raid1_arrays[slot]);

    /* Generate a simple UUID from member device IDs */
    for (int i = 0; i < 16; i++) {
        g_raid1_arrays[slot].uuid[i] = (uint8_t)(member_dev_ids[i % num_members] *
                                                  0x9e3779b9 + i);
    }

    /* Build a name like "md0" */
    char md_name[16];
    snprintf(md_name, sizeof(md_name), "md%d", slot);

    /* Register as a block device using the submit function interface */
    int ret = blockdev_register(md_id, md_name,
                                raid1_submit_fn,
                                NULL, /* idle_fn */
                                g_raid1_arrays[slot].array_sectors,
                                0);   /* flags (sync driver) */
    if (ret != 0) {
        kprintf("[mdadm] RAID1: failed to register block device md%d (ret=%d)\n",
                slot, ret);
        memset(&g_raid1_arrays[slot], 0, sizeof(struct raid1_array));
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        return -1;
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);

    kprintf("[mdadm] RAID1 array md%d created: %d members, %llu sectors, id=%d\n",
            slot, num_members,
            (unsigned long long)g_raid1_arrays[slot].array_sectors,
            md_id);

    return md_id;
}

/* ── Destroy a RAID1 array ─────────────────────────────────────────── */

void raid1_destroy(int array_id)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    for (int i = 0; i < MAX_RAID1_ARRAYS; i++) {
        if (g_raid1_arrays[i].array_id == array_id && g_raid1_arrays[i].num_members > 0) {
            int md_id = g_raid1_arrays[i].array_id;
            blockdev_unregister(md_id);
            kprintf("[mdadm] RAID1 array md%d (id=%d) destroyed\n", i, md_id);
            memset(&g_raid1_arrays[i], 0, sizeof(struct raid1_array));
            break;
        }
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);
}

/* ── Mark a member as failed ───────────────────────────────────────── */

int raid1_member_failed(int array_id, int dev_id)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    int found = 0;
    for (int i = 0; i < MAX_RAID1_ARRAYS; i++) {
        if (g_raid1_arrays[i].array_id == array_id && g_raid1_arrays[i].num_members > 0) {
            struct raid1_array *arr = &g_raid1_arrays[i];
            for (int j = 0; j < arr->num_members; j++) {
                if (arr->members[j].dev_id == dev_id &&
                    arr->members[j].state == RAID_MEMBER_ACTIVE) {
                    arr->members[j].state = RAID_MEMBER_FAILED;
                    kprintf("[mdadm] RAID1 md%d: member %d (dev_id=%d) marked FAILED\n",
                            i, j, dev_id);

                    /* Check if we still have any active members */
                    int active = 0;
                    for (int k = 0; k < arr->num_members; k++) {
                        if (arr->members[k].state == RAID_MEMBER_ACTIVE)
                            active++;
                    }
                    if (active == 0) {
                        arr->state = RAID_ARRAY_FAILED;
                        kprintf("[mdadm] RAID1 md%d: ALL members failed, array is FAILED\n", i);
                    } else {
                        arr->state = RAID_ARRAY_DEGRADED;
                        kprintf("[mdadm] RAID1 md%d: degraded (%d/%d members active)\n",
                                i, active, arr->num_members);
                    }
                    found = 1;
                    break;
                }
            }
            break;
        }
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);
    return found ? 0 : -1;
}

/* ── Query array status ────────────────────────────────────────────── */

int raid1_status(int array_id, int *state_out, int *num_members_out,
                 uint64_t *sectors_out)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    int found = 0;
    for (int i = 0; i < MAX_RAID1_ARRAYS; i++) {
        if (g_raid1_arrays[i].array_id == array_id && g_raid1_arrays[i].num_members > 0) {
            if (state_out)      *state_out      = g_raid1_arrays[i].state;
            if (num_members_out) *num_members_out = g_raid1_arrays[i].num_members;
            if (sectors_out)    *sectors_out    = g_raid1_arrays[i].array_sectors;
            found = 1;
            break;
        }
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);
    return found ? 0 : -1;
}

/* ── RAID1 block device submit function ────────────────────────────── */

/*
 * Handle a block I/O request to a RAID1 array.
 *
 * For writes: mirror to all active member devices.
 * For reads: read from the first active member; if it fails try the next.
 * Failing members are marked as failed and the array state is updated.
 */
static int raid1_submit_fn(struct blk_request *req)
{
    if (!req) return -EINVAL;

    int dev_id = req->dev_id;

    /* Find the RAID1 array for this device */
    struct raid1_array *array = NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    for (int i = 0; i < MAX_RAID1_ARRAYS; i++) {
        if (g_raid1_arrays[i].array_id == dev_id && g_raid1_arrays[i].num_members > 0) {
            array = &g_raid1_arrays[i];
            break;
        }
    }

    if (!array) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        return -ENODEV;
    }

    /* Check array state */
    if (array->state == RAID_ARRAY_FAILED) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        req->result = -EIO;
        return -EIO;
    }

    /* Save a copy of the array state to work with outside the lock */
    struct raid1_array local_array;
    memcpy(&local_array, array, sizeof(struct raid1_array));
    spinlock_irqsave_release(&g_raid_lock, irq_flags);

    uint64_t lba       = req->lba;
    uint32_t count     = req->count;
    uint8_t *buf       = (uint8_t *)req->buf;
    int      is_write  = (req->flags & BLK_REQ_WRITE);
    int      is_discard = (req->flags & BLK_REQ_DISCARD);

    int result = 0;
    int first_write_error = 0;

    if (is_discard) {
        /* DISCARD: forward to all active members */
        for (int i = 0; i < local_array.num_members; i++) {
            if (local_array.members[i].state == RAID_MEMBER_ACTIVE) {
                int r = blockdev_discard(local_array.members[i].dev_id, lba, count);
                if (r != 0 && result == 0) {
                    result = r;
                    raid1_member_failed(dev_id, local_array.members[i].dev_id);
                }
            }
        }
    } else if (is_write) {
        /* WRITE: mirror to all active members */
        for (int i = 0; i < local_array.num_members; i++) {
            if (local_array.members[i].state == RAID_MEMBER_ACTIVE) {
                int r = member_io(local_array.members[i].dev_id, lba, count, buf, 1);
                if (r != 0) {
                    /* First failure — save the error code */
                    if (first_write_error == 0)
                        first_write_error = r;
                    /* Mark this member as failed */
                    raid1_member_failed(dev_id, local_array.members[i].dev_id);
                }
            }
        }

        /* If all writes failed, return the first error.
         * If at least one write succeeded (i.e. at least one member still active),
         * consider the write successful. */
        if (first_write_error != 0) {
            uint64_t lock_flags;
            spinlock_irqsave_acquire(&g_raid_lock, &lock_flags);
            int active_count = 0;
            for (int i = 0; i < MAX_RAID1_ARRAYS; i++) {
                if (g_raid1_arrays[i].array_id == dev_id && g_raid1_arrays[i].num_members > 0) {
                    for (int j = 0; j < g_raid1_arrays[i].num_members; j++) {
                        if (g_raid1_arrays[i].members[j].state == RAID_MEMBER_ACTIVE) {
                            active_count++;
                        }
                    }
                    break;
                }
            }
            spinlock_irqsave_release(&g_raid_lock, lock_flags);
            if (active_count == 0) {
                result = -EIO;
                req->result = result;
                return result;
            }
            /* At least one write succeeded */
            result = 0;
        }
    } else {
        /* READ: try first active member; fallback on failure */
        int read_ok = 0;
        for (int i = 0; i < local_array.num_members; i++) {
            if (local_array.members[i].state == RAID_MEMBER_ACTIVE) {
                int r = member_io(local_array.members[i].dev_id, lba, count, buf, 0);
                if (r == 0) {
                    read_ok = 1;
                    break;
                }
                /* This member failed — mark it and try next */
                raid1_member_failed(dev_id, local_array.members[i].dev_id);
            }
        }

        if (!read_ok) {
            result = -EIO;
            req->result = result;
            return result;
        }
    }

    req->result = result;
    return result;
}

/* ── RAID0 set-bit (mark array as RAID0 in superblock) ───────────────── */

int raid_set_level_raid0(struct raid_super *super)
{
    if (!super) return -EINVAL;
    super->level = 0;
    super->checksum = raid_super_checksum(super);
    kprintf("[mdadm] RAID superblock set to RAID0\n");
    return 0;
}

int raid_is_raid0(const struct raid_super *super)
{
    if (!super) return 0;
    return (super->magic == RAID_SUPER_MAGIC && super->level == 0);
}

int raid_create_raid0(struct raid_super *super, uint32_t num_disks,
                      uint32_t chunk_size, uint64_t disk_sectors,
                      const uint8_t *uuid)
{
    if (!super || !uuid) return -EINVAL;

    memset(super, 0, sizeof(*super));
    super->magic = RAID_SUPER_MAGIC;
    super->level = 0;
    super->num_disks = num_disks;
    super->chunk_size = chunk_size;
    super->disk_sectors = disk_sectors;
    memcpy(super->uuid, uuid, 16);
    super->checksum = raid_super_checksum(super);

    kprintf("[mdadm] RAID0 created: %u disks, chunk=%u sectors\n",
            num_disks, chunk_size);
    return 0;
}

/* ── RAID0: Stripe Set Management ─────────────────────────────────── */

/*
 * Compute the stripe mapping for a given logical sector:
 *   disk = (lba / chunk_size) % num_disks
 *   disk_lba = (lba / chunk_size / num_disks) * chunk_size + (lba % chunk_size)
 *
 * For a multi-sector request, we iterate sector-by-sector if the
 * request crosses a stripe boundary (simple but correct).
 */

struct raid0_stripe {
    int      disk;       /* target disk index */
    uint64_t disk_lba;   /* LBA on the target disk */
};

static void raid0_stripe_map(struct raid0_array *arr, uint64_t lba,
                             struct raid0_stripe *out)
{
    uint32_t chunk = arr->chunk_size;
    int ndisks = arr->num_disks;
    
    /* Logical chunk index */
    uint64_t chunk_idx = lba / chunk;
    /* Which disk owns this chunk */
    out->disk = (int)(chunk_idx % ndisks);
    /* LBA within that disk */
    out->disk_lba = (chunk_idx / ndisks) * chunk + (lba % chunk);
}

/* ── Create a RAID0 array ─────────────────────────────────────────── */

int raid0_create(const int *member_dev_ids, int num_disks,
                 uint32_t chunk_size, const uint8_t *uuid)
{
    if (!member_dev_ids || num_disks < 2 || num_disks > RAID0_MAX_DISKS) {
        kprintf("[mdadm] RAID0: invalid parameters (%d disks, max %d)\n",
                num_disks, RAID0_MAX_DISKS);
        return -1;
    }

    if (!g_raid_initialized)
        raid_md_init();

    if (chunk_size == 0)
        chunk_size = RAID0_DEFAULT_CHUNK_SECT;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_RAID0_ARRAYS; i++) {
        if (g_raid0_arrays[i].num_disks == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        kprintf("[mdadm] RAID0: no free array slots\n");
        return -1;
    }

    /* Compute the MD block device id */
    int md_id = MD_BLOCKDEV_BASE + g_raid_next_id;
    g_raid_next_id = (g_raid_next_id + 1) % (BLOCKDEV_MAX_DEVICES - MD_BLOCKDEV_BASE);

    /* Build the array */
    memset(&g_raid0_arrays[slot], 0, sizeof(struct raid0_array));
    g_raid0_arrays[slot].array_id    = md_id;
    g_raid0_arrays[slot].state       = RAID_ARRAY_CLEAN;
    g_raid0_arrays[slot].num_disks   = num_disks;
    g_raid0_arrays[slot].chunk_size  = chunk_size;

    uint64_t min_sectors = (uint64_t)-1;
    for (int i = 0; i < num_disks; i++) {
        int dev_id = member_dev_ids[i];
        g_raid0_arrays[slot].disks[i].dev_id = dev_id;
        g_raid0_arrays[slot].disks[i].state  = RAID_MEMBER_ACTIVE;
        g_raid0_arrays[slot].disks[i].sector_count = blockdev_get_sectors(dev_id);
        if (g_raid0_arrays[slot].disks[i].sector_count < min_sectors)
            min_sectors = g_raid0_arrays[slot].disks[i].sector_count;
    }

    if (min_sectors == (uint64_t)-1 || min_sectors == 0) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        kprintf("[mdadm] RAID0: all disks have 0 sectors\n");
        memset(&g_raid0_arrays[slot], 0, sizeof(struct raid0_array));
        return -1;
    }

    g_raid0_arrays[slot].disk_sectors = min_sectors;
    g_raid0_arrays[slot].stripe_size  = (uint64_t)chunk_size * num_disks;
    /* Total array capacity: each disk contributes min_sectors, striped */
    g_raid0_arrays[slot].array_sectors = min_sectors * num_disks;

    if (uuid) {
        memcpy(g_raid0_arrays[slot].uuid, uuid, 16);
    } else {
        for (int i = 0; i < 16; i++)
            g_raid0_arrays[slot].uuid[i] = (uint8_t)(member_dev_ids[i % num_disks] *
                                                      0x9e3779b9 + i + 0x100);
    }

    /* Build a name like "md0" */
    char md_name[16];
    snprintf(md_name, sizeof(md_name), "md%d", slot + MAX_RAID1_ARRAYS);

    /* Register as a block device */
    int ret = blockdev_register(md_id, md_name,
                                raid0_submit_fn,
                                NULL,
                                g_raid0_arrays[slot].array_sectors,
                                0);
    if (ret != 0) {
        kprintf("[mdadm] RAID0: failed to register block device %s (ret=%d)\n",
                md_name, ret);
        memset(&g_raid0_arrays[slot], 0, sizeof(struct raid0_array));
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        return -1;
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);

    kprintf("[mdadm] RAID0 array %s created: %d disks, chunk=%u sectors, "
            "%llu total sectors, id=%d\n",
            md_name, num_disks, chunk_size,
            (unsigned long long)g_raid0_arrays[slot].array_sectors,
            md_id);

    return md_id;
}

/* ── Destroy a RAID0 array ────────────────────────────────────────── */

void raid0_destroy(int array_id)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    for (int i = 0; i < MAX_RAID0_ARRAYS; i++) {
        if (g_raid0_arrays[i].array_id == array_id && g_raid0_arrays[i].num_disks > 0) {
            blockdev_unregister(array_id);
            kprintf("[mdadm] RAID0 array md%d (id=%d) destroyed\n",
                    i + MAX_RAID1_ARRAYS, array_id);
            memset(&g_raid0_arrays[i], 0, sizeof(struct raid0_array));
            break;
        }
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);
}

/* ── RAID0 status query ───────────────────────────────────────────── */

int raid0_status(int array_id, int *state_out, int *num_disks_out,
                 uint64_t *sectors_out, uint32_t *chunk_out)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    int found = 0;
    for (int i = 0; i < MAX_RAID0_ARRAYS; i++) {
        if (g_raid0_arrays[i].array_id == array_id && g_raid0_arrays[i].num_disks > 0) {
            if (state_out)     *state_out     = g_raid0_arrays[i].state;
            if (num_disks_out) *num_disks_out = g_raid0_arrays[i].num_disks;
            if (sectors_out)   *sectors_out   = g_raid0_arrays[i].array_sectors;
            if (chunk_out)     *chunk_out     = g_raid0_arrays[i].chunk_size;
            found = 1;
            break;
        }
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);
    return found ? 0 : -1;
}

/* ── RAID0 block device submit function ───────────────────────────── */

static int raid0_submit_fn(struct blk_request *req)
{
    if (!req) return -EINVAL;

    int dev_id = req->dev_id;

    /* Find the RAID0 array for this device */
    struct raid0_array *array = NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    for (int i = 0; i < MAX_RAID0_ARRAYS; i++) {
        if (g_raid0_arrays[i].array_id == dev_id && g_raid0_arrays[i].num_disks > 0) {
            array = &g_raid0_arrays[i];
            break;
        }
    }

    if (!array) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        return -ENODEV;
    }

    if (array->state == RAID_ARRAY_FAILED) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        req->result = -EIO;
        return -EIO;
    }

    /* Save a copy to work outside the lock */
    struct raid0_array local_array;
    memcpy(&local_array, array, sizeof(struct raid0_array));
    spinlock_irqsave_release(&g_raid_lock, irq_flags);

    uint64_t lba       = req->lba;
    uint32_t count     = req->count;
    uint8_t *buf       = (uint8_t *)req->buf;
    int      is_write  = (req->flags & BLK_REQ_WRITE);
    int      is_discard = (req->flags & BLK_REQ_DISCARD);

    int result = 0;

    if (is_discard) {
        /* DISCARD: forward to the appropriate disk for each sector range */
        uint64_t remaining = count;
        uint64_t current_lba = lba;

        while (remaining > 0) {
            struct raid0_stripe stripe;
            raid0_stripe_map(&local_array, current_lba, &stripe);

            /* Determine how many contiguous sectors on this disk */
            uint64_t chunk_remain = local_array.chunk_size -
                                    (current_lba % local_array.chunk_size);
            uint64_t this_count = (remaining < chunk_remain) ? remaining : chunk_remain;

            if (stripe.disk >= 0 && stripe.disk < local_array.num_disks &&
                local_array.disks[stripe.disk].state == RAID_MEMBER_ACTIVE) {
                int r = blockdev_discard(local_array.disks[stripe.disk].dev_id,
                                         stripe.disk_lba, (uint32_t)this_count);
                if (r != 0 && result == 0)
                    result = r;
            }

            current_lba += this_count;
            remaining -= this_count;
        }
    } else if (is_write) {
        /* WRITE: stripe across disks */
        uint64_t remaining = count;
        uint64_t current_lba = lba;
        uint64_t buf_offset = 0;

        while (remaining > 0) {
            struct raid0_stripe stripe;
            raid0_stripe_map(&local_array, current_lba, &stripe);

            /* Sectors remaining in this chunk */
            uint64_t chunk_remain = local_array.chunk_size -
                                    (current_lba % local_array.chunk_size);
            uint64_t this_count = (remaining < chunk_remain) ? remaining : chunk_remain;

            if (stripe.disk >= 0 && stripe.disk < local_array.num_disks &&
                local_array.disks[stripe.disk].state == RAID_MEMBER_ACTIVE) {
                int r = member_io(local_array.disks[stripe.disk].dev_id,
                                  stripe.disk_lba, (uint32_t)this_count,
                                  buf + buf_offset, 1);
                if (r != 0) {
                    /* Mark member as failed */
                    raid1_member_failed(dev_id, local_array.disks[stripe.disk].dev_id);
                    if (result == 0) result = r;
                }
            }

            current_lba += this_count;
            remaining   -= this_count;
            buf_offset  += this_count * 512;
        }
    } else {
        /* READ: stripe across disks */
        uint64_t remaining = count;
        uint64_t current_lba = lba;
        uint64_t buf_offset = 0;

        while (remaining > 0) {
            struct raid0_stripe stripe;
            raid0_stripe_map(&local_array, current_lba, &stripe);

            uint64_t chunk_remain = local_array.chunk_size -
                                    (current_lba % local_array.chunk_size);
            uint64_t this_count = (remaining < chunk_remain) ? remaining : chunk_remain;

            if (stripe.disk >= 0 && stripe.disk < local_array.num_disks &&
                local_array.disks[stripe.disk].state == RAID_MEMBER_ACTIVE) {
                int r = member_io(local_array.disks[stripe.disk].dev_id,
                                  stripe.disk_lba, (uint32_t)this_count,
                                  buf + buf_offset, 0);
                if (r != 0) {
                    raid1_member_failed(dev_id, local_array.disks[stripe.disk].dev_id);
                    if (result == 0) result = r;
                }
            } else {
                if (result == 0) result = -EIO;
            }

            current_lba += this_count;
            remaining   -= this_count;
            buf_offset  += this_count * 512;
        }
    }

    req->result = result;
    return result;
}

/* ── RAID10: Stripe of Mirrors (RAID1+0) ──────────────────────────── */

/*
 * RAID10 = RAID0 across N/2 mirror pairs.
 *
 * For a write: write to both members of the mirror pair (like RAID1).
 * For a read: read from the primary member of the pair (or secondary if primary fails).
 *
 * Stripe mapping: pairs[stripe_index] where stripe_index = (LBA / chunk) % num_pairs.
 */

struct raid10_stripe {
    int      pair;        /* mirror pair index */
    uint64_t pair_lba;    /* LBA within the mirror pair */
};

static void raid10_stripe_map(struct raid10_array *arr, uint64_t lba,
                              struct raid10_stripe *out)
{
    uint32_t chunk = arr->chunk_size;
    int npairs = arr->num_pairs;

    uint64_t chunk_idx = lba / chunk;
    out->pair = (int)(chunk_idx % npairs);
    out->pair_lba = (chunk_idx / npairs) * chunk + (lba % chunk);
}

/* ── Create a RAID10 array ────────────────────────────────────────── */

int raid10_create(const int *member_dev_ids, int num_disks,
                  uint32_t chunk_size, const uint8_t *uuid)
{
    if (!member_dev_ids || num_disks < 4 || num_disks > RAID10_MAX_DISKS ||
        (num_disks & 1)) {
        kprintf("[mdadm] RAID10: need even number of disks (>=4), got %d\n", num_disks);
        return -1;
    }

    if (!g_raid_initialized)
        raid_md_init();

    if (chunk_size == 0)
        chunk_size = RAID0_DEFAULT_CHUNK_SECT;

    int num_pairs = num_disks / 2;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_RAID10_ARRAYS; i++) {
        if (g_raid10_arrays[i].num_pairs == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        kprintf("[mdadm] RAID10: no free array slots\n");
        return -1;
    }

    int md_id = MD_BLOCKDEV_BASE + g_raid_next_id;
    g_raid_next_id = (g_raid_next_id + 1) % (BLOCKDEV_MAX_DEVICES - MD_BLOCKDEV_BASE);

    memset(&g_raid10_arrays[slot], 0, sizeof(struct raid10_array));
    g_raid10_arrays[slot].array_id   = md_id;
    g_raid10_arrays[slot].state      = RAID_ARRAY_CLEAN;
    g_raid10_arrays[slot].num_pairs  = num_pairs;
    g_raid10_arrays[slot].chunk_size = chunk_size;

    uint64_t min_pair_sectors = (uint64_t)-1;

    for (int p = 0; p < num_pairs; p++) {
        int primary_id   = member_dev_ids[p * 2];
        int secondary_id = member_dev_ids[p * 2 + 1];

        g_raid10_arrays[slot].pairs[p].primary.dev_id   = primary_id;
        g_raid10_arrays[slot].pairs[p].primary.state    = RAID_MEMBER_ACTIVE;
        g_raid10_arrays[slot].pairs[p].primary.sector_count = blockdev_get_sectors(primary_id);

        g_raid10_arrays[slot].pairs[p].secondary.dev_id   = secondary_id;
        g_raid10_arrays[slot].pairs[p].secondary.state    = RAID_MEMBER_ACTIVE;
        g_raid10_arrays[slot].pairs[p].secondary.sector_count = blockdev_get_sectors(secondary_id);

        uint64_t pair_sectors = g_raid10_arrays[slot].pairs[p].primary.sector_count;
        if (g_raid10_arrays[slot].pairs[p].secondary.sector_count < pair_sectors)
            pair_sectors = g_raid10_arrays[slot].pairs[p].secondary.sector_count;

        if (pair_sectors < min_pair_sectors)
            min_pair_sectors = pair_sectors;
    }

    if (min_pair_sectors == (uint64_t)-1 || min_pair_sectors == 0) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        kprintf("[mdadm] RAID10: all members have 0 sectors\n");
        memset(&g_raid10_arrays[slot], 0, sizeof(struct raid10_array));
        return -1;
    }

    g_raid10_arrays[slot].pair_sectors  = min_pair_sectors;
    g_raid10_arrays[slot].stripe_size   = (uint64_t)chunk_size * num_pairs;
    g_raid10_arrays[slot].array_sectors = min_pair_sectors * num_pairs;

    if (uuid) {
        memcpy(g_raid10_arrays[slot].uuid, uuid, 16);
    } else {
        for (int i = 0; i < 16; i++)
            g_raid10_arrays[slot].uuid[i] = (uint8_t)(member_dev_ids[i % num_disks] *
                                                        0x9e3779b9 + i + 0x200);
    }

    char md_name[16];
    snprintf(md_name, sizeof(md_name), "md%d", slot + MAX_RAID1_ARRAYS + MAX_RAID0_ARRAYS);

    int ret = blockdev_register(md_id, md_name,
                                raid10_submit_fn,
                                NULL,
                                g_raid10_arrays[slot].array_sectors,
                                0);
    if (ret != 0) {
        kprintf("[mdadm] RAID10: failed to register block device %s (ret=%d)\n",
                md_name, ret);
        memset(&g_raid10_arrays[slot], 0, sizeof(struct raid10_array));
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        return -1;
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);

    kprintf("[mdadm] RAID10 array %s created: %d pairs (striped mirrors), "
            "chunk=%u sectors, %llu total sectors, id=%d\n",
            md_name, num_pairs, chunk_size,
            (unsigned long long)g_raid10_arrays[slot].array_sectors,
            md_id);

    return md_id;
}

/* ── Destroy a RAID10 array ───────────────────────────────────────── */

void raid10_destroy(int array_id)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    for (int i = 0; i < MAX_RAID10_ARRAYS; i++) {
        if (g_raid10_arrays[i].array_id == array_id && g_raid10_arrays[i].num_pairs > 0) {
            blockdev_unregister(array_id);
            kprintf("[mdadm] RAID10 array md%d (id=%d) destroyed\n",
                    i + MAX_RAID1_ARRAYS + MAX_RAID0_ARRAYS, array_id);
            memset(&g_raid10_arrays[i], 0, sizeof(struct raid10_array));
            break;
        }
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);
}

/* ── RAID10 status query ──────────────────────────────────────────── */

int raid10_status(int array_id, int *state_out, int *num_pairs_out,
                  uint64_t *sectors_out, uint32_t *chunk_out)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    int found = 0;
    for (int i = 0; i < MAX_RAID10_ARRAYS; i++) {
        if (g_raid10_arrays[i].array_id == array_id && g_raid10_arrays[i].num_pairs > 0) {
            if (state_out)     *state_out     = g_raid10_arrays[i].state;
            if (num_pairs_out) *num_pairs_out = g_raid10_arrays[i].num_pairs;
            if (sectors_out)   *sectors_out   = g_raid10_arrays[i].array_sectors;
            if (chunk_out)     *chunk_out     = g_raid10_arrays[i].chunk_size;
            found = 1;
            break;
        }
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);
    return found ? 0 : -1;
}

/* ── RAID10 block device submit function ──────────────────────────── */

/*
 * For each stripe chunk:
 *   - Identify which mirror pair owns it (via stripe map)
 *   - Write: write to both primary and secondary (mirror)
 *   - Read: try primary first, fallback to secondary
 *   - Discard: forward to both members of the pair
 */
static int raid10_submit_fn(struct blk_request *req)
{
    if (!req) return -EINVAL;

    int dev_id = req->dev_id;
    struct raid10_array *array = NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    for (int i = 0; i < MAX_RAID10_ARRAYS; i++) {
        if (g_raid10_arrays[i].array_id == dev_id && g_raid10_arrays[i].num_pairs > 0) {
            array = &g_raid10_arrays[i];
            break;
        }
    }

    if (!array) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        return -ENODEV;
    }

    if (array->state == RAID_ARRAY_FAILED) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        req->result = -EIO;
        return -EIO;
    }

    struct raid10_array local_array;
    memcpy(&local_array, array, sizeof(struct raid10_array));
    spinlock_irqsave_release(&g_raid_lock, irq_flags);

    uint64_t lba       = req->lba;
    uint32_t count     = req->count;
    uint8_t *buf       = (uint8_t *)req->buf;
    int      is_write  = (req->flags & BLK_REQ_WRITE);
    int      is_discard = (req->flags & BLK_REQ_DISCARD);

    int result = 0;

    if (is_discard) {
        /* DISCARD: forward to both members of each affected pair */
        uint64_t remaining = count;
        uint64_t current_lba = lba;

        while (remaining > 0) {
            struct raid10_stripe stripe;
            raid10_stripe_map(&local_array, current_lba, &stripe);

            uint64_t chunk_remain = local_array.chunk_size -
                                    (current_lba % local_array.chunk_size);
            uint64_t this_count = (remaining < chunk_remain) ? remaining : chunk_remain;

            if (stripe.pair >= 0 && stripe.pair < local_array.num_pairs) {
                struct raid10_mirror_pair *pair = &local_array.pairs[stripe.pair];

                if (pair->primary.state == RAID_MEMBER_ACTIVE) {
                    int r = blockdev_discard(pair->primary.dev_id,
                                             stripe.pair_lba, (uint32_t)this_count);
                    if (r != 0 && result == 0) result = r;
                }
                if (pair->secondary.state == RAID_MEMBER_ACTIVE) {
                    int r = blockdev_discard(pair->secondary.dev_id,
                                             stripe.pair_lba, (uint32_t)this_count);
                    if (r != 0 && result == 0) result = r;
                }
            }

            current_lba += this_count;
            remaining -= this_count;
        }
    } else if (is_write) {
        /* WRITE: write to both members of each affected mirror pair */
        uint64_t remaining = count;
        uint64_t current_lba = lba;
        uint64_t buf_offset = 0;

        while (remaining > 0) {
            struct raid10_stripe stripe;
            raid10_stripe_map(&local_array, current_lba, &stripe);

            uint64_t chunk_remain = local_array.chunk_size -
                                    (current_lba % local_array.chunk_size);
            uint64_t this_count = (remaining < chunk_remain) ? remaining : chunk_remain;

            if (stripe.pair >= 0 && stripe.pair < local_array.num_pairs) {
                struct raid10_mirror_pair *pair = &local_array.pairs[stripe.pair];

                /* Write to primary */
                if (pair->primary.state == RAID_MEMBER_ACTIVE) {
                    int r = member_io(pair->primary.dev_id, stripe.pair_lba,
                                      (uint32_t)this_count, buf + buf_offset, 1);
                    if (r != 0) {
                        if (result == 0) result = r;
                    }
                }
                /* Write to secondary (mirror) */
                if (pair->secondary.state == RAID_MEMBER_ACTIVE) {
                    int r = member_io(pair->secondary.dev_id, stripe.pair_lba,
                                      (uint32_t)this_count, buf + buf_offset, 1);
                    if (r != 0) {
                        if (result == 0) result = r;
                    }
                }
            }

            current_lba += this_count;
            remaining   -= this_count;
            buf_offset  += this_count * 512;
        }

        /* If the entire write failed (both sides of every pair dead) */
        if (result != 0) {
            req->result = result;
            return result;
        }
    } else {
        /* READ: try primary, fallback to secondary for each stripe chunk */
        uint64_t remaining = count;
        uint64_t current_lba = lba;
        uint64_t buf_offset = 0;

        while (remaining > 0) {
            struct raid10_stripe stripe;
            raid10_stripe_map(&local_array, current_lba, &stripe);

            uint64_t chunk_remain = local_array.chunk_size -
                                    (current_lba % local_array.chunk_size);
            uint64_t this_count = (remaining < chunk_remain) ? remaining : chunk_remain;

            int read_ok = 0;

            if (stripe.pair >= 0 && stripe.pair < local_array.num_pairs) {
                struct raid10_mirror_pair *pair = &local_array.pairs[stripe.pair];

                /* Try primary */
                if (pair->primary.state == RAID_MEMBER_ACTIVE) {
                    int r = member_io(pair->primary.dev_id, stripe.pair_lba,
                                      (uint32_t)this_count, buf + buf_offset, 0);
                    if (r == 0) {
                        read_ok = 1;
                    }
                }

                /* Fallback to secondary */
                if (!read_ok && pair->secondary.state == RAID_MEMBER_ACTIVE) {
                    int r = member_io(pair->secondary.dev_id, stripe.pair_lba,
                                      (uint32_t)this_count, buf + buf_offset, 0);
                    if (r == 0) {
                        read_ok = 1;
                    }
                }
            }

            if (!read_ok) {
                if (result == 0) result = -EIO;
            }

            current_lba += this_count;
            remaining   -= this_count;
            buf_offset  += this_count * 512;
        }
    }

    req->result = result;
    return result;
}

/* ── RAID5: Rotating Parity ─────────────────────────────────────────── */

/* Compute which disk holds parity for a given stripe (LR=left-asymmetric) */
static int raid5_parity_disk(struct raid5_array *arr, uint64_t stripe_idx)
{
    /* Left-asymmetric: parity rotates left one disk per stripe */
    return (int)((arr->num_disks - 1) - (stripe_idx % arr->num_disks));
}

/* Map logical sector to (disk, disk_lba). Stripe index = lba / chunk_size.
 * Data disk = (stripe_idx % (num_disks-1)) adjusted for parity position. */
static __attribute__((unused)) int raid5_stripe_map(struct raid5_array *arr, uint64_t lba,
                            int *disk_out, uint64_t *disk_lba_out)
{
    uint32_t chunk = arr->chunk_size;
    uint64_t stripe_idx = lba / chunk;          /* stripe number */
    uint64_t stripe_offset = lba % chunk;       /* offset within stripe data */
    int data_disks = arr->num_disks - 1;        /* data disks per stripe */
    int parity = raid5_parity_disk(arr, stripe_idx);

    /* Data disk index (skipping the parity disk) */
    int data_disk = (int)(stripe_idx % data_disks);
    if (data_disk >= parity) data_disk++;

    uint64_t disk_lba = (stripe_idx / data_disks) * chunk + stripe_offset;

    *disk_out = data_disk;
    *disk_lba_out = disk_lba;
    return parity;
}

/* XOR parity computation over sector-aligned buffers */
static void raid5_xor_block(void *target, const void *src, int bytes)
{
    uint64_t *t = (uint64_t *)target;
    const uint64_t *s = (const uint64_t *)src;
    for (int i = 0; i < bytes / 8; i++)
        t[i] ^= s[i];
}

/* Create RAID5 array */
int raid5_create(const int *member_dev_ids, int num_disks,
                 uint32_t chunk_size, const uint8_t *uuid)
{
    if (!member_dev_ids || num_disks < 3 || num_disks > RAID5_MAX_DISKS) {
        kprintf("[mdadm] RAID5: invalid parameters (%d disks, need 3-%d)\n",
                num_disks, RAID5_MAX_DISKS);
        return -1;
    }

    if (!g_raid_initialized) raid_md_init();

    if (chunk_size == 0) chunk_size = RAID5_DEFAULT_CHUNK_SECT;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    int slot = -1;
    for (int i = 0; i < MAX_RAID5_ARRAYS; i++) {
        if (g_raid5_arrays[i].num_disks == 0) { slot = i; break; }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        kprintf("[mdadm] RAID5: no free array slots\n");
        return -1;
    }

    int md_id = MD_BLOCKDEV_BASE + g_raid_next_id;
    g_raid_next_id = (g_raid_next_id + 1) % (BLOCKDEV_MAX_DEVICES - MD_BLOCKDEV_BASE);

    memset(&g_raid5_arrays[slot], 0, sizeof(struct raid5_array));
    g_raid5_arrays[slot].array_id    = md_id;
    g_raid5_arrays[slot].state       = RAID_ARRAY_CLEAN;
    g_raid5_arrays[slot].num_disks   = num_disks;
    g_raid5_arrays[slot].chunk_size  = chunk_size;

    uint64_t min_sectors = (uint64_t)-1;
    for (int i = 0; i < num_disks; i++) {
        int dev_id = member_dev_ids[i];
        g_raid5_arrays[slot].disks[i].dev_id = dev_id;
        g_raid5_arrays[slot].disks[i].state = RAID_MEMBER_ACTIVE;
        g_raid5_arrays[slot].disks[i].sector_count = blockdev_get_sectors(dev_id);
        if (g_raid5_arrays[slot].disks[i].sector_count == 0) {
            spinlock_irqsave_release(&g_raid_lock, irq_flags);
            memset(&g_raid5_arrays[slot], 0, sizeof(struct raid5_array));
            return -1;
        }
        if (g_raid5_arrays[slot].disks[i].sector_count < min_sectors)
            min_sectors = g_raid5_arrays[slot].disks[i].sector_count;
    }

    g_raid5_arrays[slot].disk_sectors  = min_sectors;
    g_raid5_arrays[slot].stripe_size   = (uint64_t)chunk_size * (num_disks - 1);
    g_raid5_arrays[slot].array_sectors = min_sectors * (num_disks - 1);

    if (uuid) {
        memcpy(g_raid5_arrays[slot].uuid, uuid, 16);
    } else {
        for (int i = 0; i < 16; i++)
            g_raid5_arrays[slot].uuid[i] = (uint8_t)(member_dev_ids[i % num_disks] *
                                                        0x9e3779b9 + i + 0x300);
    }

    char md_name[16];
    snprintf(md_name, sizeof(md_name), "md%d",
             slot + MAX_RAID1_ARRAYS + MAX_RAID0_ARRAYS + MAX_RAID10_ARRAYS);

    int ret = blockdev_register(md_id, md_name,
                                raid5_submit_fn, NULL,
                                g_raid5_arrays[slot].array_sectors, 0);
    if (ret != 0) {
        kprintf("[mdadm] RAID5: failed to register %s (ret=%d)\n", md_name, ret);
        memset(&g_raid5_arrays[slot], 0, sizeof(struct raid5_array));
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        return -1;
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);
    kprintf("[mdadm] RAID5 array %s: %d disks, chunk=%u sectors, "
            "%llu total sectors, id=%d\n",
            md_name, num_disks, chunk_size,
            (unsigned long long)g_raid5_arrays[slot].array_sectors, md_id);
    return md_id;
}

void raid5_destroy(int array_id)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);
    for (int i = 0; i < MAX_RAID5_ARRAYS; i++) {
        if (g_raid5_arrays[i].array_id == array_id && g_raid5_arrays[i].num_disks > 0) {
            blockdev_unregister(array_id);
            kprintf("[mdadm] RAID5 array md%d (id=%d) destroyed\n", i, array_id);
            memset(&g_raid5_arrays[i], 0, sizeof(struct raid5_array));
            break;
        }
    }
    spinlock_irqsave_release(&g_raid_lock, irq_flags);
}

int raid5_status(int array_id, int *state_out, int *num_disks_out,
                 uint64_t *sectors_out, uint32_t *chunk_out)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);
    int found = 0;
    for (int i = 0; i < MAX_RAID5_ARRAYS; i++) {
        if (g_raid5_arrays[i].array_id == array_id && g_raid5_arrays[i].num_disks > 0) {
            if (state_out)     *state_out     = g_raid5_arrays[i].state;
            if (num_disks_out) *num_disks_out = g_raid5_arrays[i].num_disks;
            if (sectors_out)   *sectors_out   = g_raid5_arrays[i].array_sectors;
            if (chunk_out)     *chunk_out     = g_raid5_arrays[i].chunk_size;
            found = 1; break;
        }
    }
    spinlock_irqsave_release(&g_raid_lock, irq_flags);
    return found ? 0 : -1;
}

/* RAID5 submit function */
static int raid5_submit_fn(struct blk_request *req)
{
    if (!req) return -EINVAL;
    int dev_id = req->dev_id;
    struct raid5_array *array = NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);
    for (int i = 0; i < MAX_RAID5_ARRAYS; i++) {
        if (g_raid5_arrays[i].array_id == dev_id && g_raid5_arrays[i].num_disks > 0) {
            array = &g_raid5_arrays[i];
            break;
        }
    }
    if (!array) { spinlock_irqsave_release(&g_raid_lock, irq_flags); return -ENODEV; }
    if (array->state == RAID_ARRAY_FAILED) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        req->result = -EIO; return -EIO;
    }
    struct raid5_array local_array;
    memcpy(&local_array, array, sizeof(struct raid5_array));
    spinlock_irqsave_release(&g_raid_lock, irq_flags);

    uint64_t lba = req->lba;
    uint32_t count = req->count;
    uint8_t *buf = (uint8_t *)req->buf;
    int is_write = (req->flags & BLK_REQ_WRITE);
    uint32_t chunk = local_array.chunk_size;
    int nd = local_array.num_disks;
    int data_disks = nd - 1;
    int result = 0;

    /* Per-sector processing for simplicity and correctness */
    for (uint32_t s = 0; s < count; s++) {
        uint64_t current_lba = lba + s;
        uint64_t stripe_idx = current_lba / chunk;
        uint64_t stripe_off = current_lba % chunk;
        int parity = raid5_parity_disk(&local_array, stripe_idx);
        int data_disk = (int)(stripe_idx % data_disks);
        if (data_disk >= parity) data_disk++;
        uint64_t disk_lba = (stripe_idx / data_disks) * chunk + stripe_off;

        if (is_write) {
            /* Read-modify-write: read old data and old parity, compute new parity */
            uint8_t old_data[512], old_parity[512], new_data[512];
            memcpy(new_data, buf + s * 512, 512);

            /* Read old data from the target disk */
            int r = member_io(local_array.disks[data_disk].dev_id, disk_lba, 1, old_data, 0);
            if (r != 0) {
                /* Degraded mode: reconstruct from parity */
                memset(old_data, 0, 512);
                for (int d = 0; d < nd; d++) {
                    if (d == parity || d == data_disk) continue;
                    if (local_array.disks[d].state == RAID_MEMBER_ACTIVE) {
                        uint8_t tmp[512];
                        if (member_io(local_array.disks[d].dev_id, disk_lba, 1, tmp, 0) == 0)
                            raid5_xor_block(old_data, tmp, 512);
                    }
                }
            }

            /* Read old parity from parity disk */
            r = member_io(local_array.disks[parity].dev_id, disk_lba, 1, old_parity, 0);
            if (r != 0) memset(old_parity, 0, 512);

            /* Compute new parity: old_parity ^ old_data ^ new_data */
            raid5_xor_block(old_parity, old_data, 512);
            raid5_xor_block(old_parity, new_data, 512);

            /* Write new data */
            r = member_io(local_array.disks[data_disk].dev_id, disk_lba, 1, new_data, 1);
            if (r != 0) { if (result == 0) result = r; }

            /* Write new parity */
            r = member_io(local_array.disks[parity].dev_id, disk_lba, 1, old_parity, 1);
            if (r != 0) { if (result == 0) result = r; }
        } else {
            /* READ: try data disk first, reconstruct from parity if failed */
            int read_ok = 0;
            if (local_array.disks[data_disk].state == RAID_MEMBER_ACTIVE) {
                int r = member_io(local_array.disks[data_disk].dev_id, disk_lba, 1,
                                  buf + s * 512, 0);
                if (r == 0) read_ok = 1;
            }
            if (!read_ok) {
                /* Reconstruct from parity: XOR all other disks */
                uint8_t *out = buf + s * 512;
                memset(out, 0, 512);
                for (int d = 0; d < nd; d++) {
                    if (d == data_disk) continue;
                    if (local_array.disks[d].state == RAID_MEMBER_ACTIVE) {
                        uint8_t tmp[512];
                        if (member_io(local_array.disks[d].dev_id, disk_lba, 1, tmp, 0) == 0)
                            raid5_xor_block(out, tmp, 512);
                    }
                }
                read_ok = 1; /* degraded but we reconstructed */
            }
            if (!read_ok && result == 0) result = -EIO;
        }
    }

    req->result = result;
    return result;
}

/* ── RAID6: Double Parity P+Q ────────────────────────────────────────── */

/* Galois field operations for RAID6 Q parity */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1D; /* GF(2^8) irreducible polynomial x^8+x^4+x^3+x^2+1 */
        b >>= 1;
    }
    return p;
}

/* Compute Q syndrome using Galois field multiplication by powers of 2 */
static void raid6_q_syndrome(uint8_t *q, const uint8_t *data, int len, int index)
{
    /* q ^= gf_mul(data, 2^index) */
    for (int i = 0; i < len; i++) {
        q[i] ^= gf_mul(data[i], (uint8_t)(1 << (index & 7)));
    }
}

/* Compute P and Q for a stripe */
static __attribute__((unused)) void raid6_compute_pq(uint8_t *p, uint8_t *q, const uint8_t *chunks,
                             int chunk_bytes, int num_data_disks)
{
    memset(p, 0, chunk_bytes);
    memset(q, 0, chunk_bytes);
    for (int d = 0; d < num_data_disks; d++) {
        const uint8_t *data = chunks + d * chunk_bytes;
        raid5_xor_block(p, data, chunk_bytes);
        raid6_q_syndrome(q, data, chunk_bytes, d);
    }
}

/* Map RAID6 stripe: which disks hold P and Q */
static void raid6_pq_disks(struct raid6_array *arr, uint64_t stripe_idx,
                            int *p_disk, int *q_disk)
{
    int nd = arr->num_disks;
    /* Left-symmetric: P at (nd-1 - (stripe_idx % nd)), Q at (P-1 mod nd) */
    *p_disk = (int)((nd - 1) - (stripe_idx % nd));
    *q_disk = (*p_disk == 0) ? (nd - 1) : (*p_disk - 1);
}

/* Map logical sector to (data_disk, disk_lba) for RAID6 */
static int raid6_data_disk(struct raid6_array *arr, uint64_t stripe_idx,
                            uint64_t stripe_off, int p_disk, int q_disk,
                            uint64_t *disk_lba_out)
{
    int data_disks = arr->num_disks - 2;
    uint32_t chunk = arr->chunk_size;
    int data_disk = (int)(stripe_idx % data_disks);
    /* Adjust for P and Q positions */
    int offset = 0;
    for (int d = 0; d < arr->num_disks; d++) {
        if (d == p_disk || d == q_disk) continue;
        if (offset == data_disk) {
            *disk_lba_out = (stripe_idx / data_disks) * chunk + stripe_off;
            return d;
        }
        offset++;
    }
    *disk_lba_out = (stripe_idx / data_disks) * chunk + stripe_off;
    return data_disk;
}

int raid6_create(const int *member_dev_ids, int num_disks,
                 uint32_t chunk_size, const uint8_t *uuid)
{
    if (!member_dev_ids || num_disks < 4 || num_disks > RAID6_MAX_DISKS) {
        kprintf("[mdadm] RAID6: invalid parameters (%d disks, need 4-%d)\n",
                num_disks, RAID6_MAX_DISKS);
        return -1;
    }

    if (!g_raid_initialized) raid_md_init();

    if (chunk_size == 0) chunk_size = RAID6_DEFAULT_CHUNK_SECT;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);

    int slot = -1;
    for (int i = 0; i < MAX_RAID6_ARRAYS; i++) {
        if (g_raid6_arrays[i].num_disks == 0) { slot = i; break; }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        kprintf("[mdadm] RAID6: no free array slots\n");
        return -1;
    }

    int md_id = MD_BLOCKDEV_BASE + g_raid_next_id;
    g_raid_next_id = (g_raid_next_id + 1) % (BLOCKDEV_MAX_DEVICES - MD_BLOCKDEV_BASE);

    memset(&g_raid6_arrays[slot], 0, sizeof(struct raid6_array));
    g_raid6_arrays[slot].array_id    = md_id;
    g_raid6_arrays[slot].state       = RAID_ARRAY_CLEAN;
    g_raid6_arrays[slot].num_disks   = num_disks;
    g_raid6_arrays[slot].chunk_size  = chunk_size;

    uint64_t min_sectors = (uint64_t)-1;
    for (int i = 0; i < num_disks; i++) {
        int dev_id = member_dev_ids[i];
        g_raid6_arrays[slot].disks[i].dev_id = dev_id;
        g_raid6_arrays[slot].disks[i].state = RAID_MEMBER_ACTIVE;
        g_raid6_arrays[slot].disks[i].sector_count = blockdev_get_sectors(dev_id);
        if (g_raid6_arrays[slot].disks[i].sector_count == 0) {
            spinlock_irqsave_release(&g_raid_lock, irq_flags);
            memset(&g_raid6_arrays[slot], 0, sizeof(struct raid6_array));
            return -1;
        }
        if (g_raid6_arrays[slot].disks[i].sector_count < min_sectors)
            min_sectors = g_raid6_arrays[slot].disks[i].sector_count;
    }

    g_raid6_arrays[slot].disk_sectors  = min_sectors;
    g_raid6_arrays[slot].stripe_size   = (uint64_t)chunk_size * (num_disks - 2);
    g_raid6_arrays[slot].array_sectors = min_sectors * (num_disks - 2);

    if (uuid) {
        memcpy(g_raid6_arrays[slot].uuid, uuid, 16);
    } else {
        for (int i = 0; i < 16; i++)
            g_raid6_arrays[slot].uuid[i] = (uint8_t)(member_dev_ids[i % num_disks] *
                                                        0x9e3779b9 + i + 0x400);
    }

    char md_name[16];
    snprintf(md_name, sizeof(md_name), "md%d",
             slot + MAX_RAID1_ARRAYS + MAX_RAID0_ARRAYS + MAX_RAID10_ARRAYS + MAX_RAID5_ARRAYS);

    int ret = blockdev_register(md_id, md_name,
                                raid6_submit_fn, NULL,
                                g_raid6_arrays[slot].array_sectors, 0);
    if (ret != 0) {
        kprintf("[mdadm] RAID6: failed to register %s (ret=%d)\n", md_name, ret);
        memset(&g_raid6_arrays[slot], 0, sizeof(struct raid6_array));
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        return -1;
    }

    spinlock_irqsave_release(&g_raid_lock, irq_flags);
    kprintf("[mdadm] RAID6 array %s: %d disks, chunk=%u sectors, "
            "%llu total sectors, id=%d\n",
            md_name, num_disks, chunk_size,
            (unsigned long long)g_raid6_arrays[slot].array_sectors, md_id);
    return md_id;
}

void raid6_destroy(int array_id)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);
    for (int i = 0; i < MAX_RAID6_ARRAYS; i++) {
        if (g_raid6_arrays[i].array_id == array_id && g_raid6_arrays[i].num_disks > 0) {
            blockdev_unregister(array_id);
            kprintf("[mdadm] RAID6 array md%d (id=%d) destroyed\n", i, array_id);
            memset(&g_raid6_arrays[i], 0, sizeof(struct raid6_array));
            break;
        }
    }
    spinlock_irqsave_release(&g_raid_lock, irq_flags);
}

int raid6_status(int array_id, int *state_out, int *num_disks_out,
                 uint64_t *sectors_out, uint32_t *chunk_out)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);
    int found = 0;
    for (int i = 0; i < MAX_RAID6_ARRAYS; i++) {
        if (g_raid6_arrays[i].array_id == array_id && g_raid6_arrays[i].num_disks > 0) {
            if (state_out)     *state_out     = g_raid6_arrays[i].state;
            if (num_disks_out) *num_disks_out = g_raid6_arrays[i].num_disks;
            if (sectors_out)   *sectors_out   = g_raid6_arrays[i].array_sectors;
            if (chunk_out)     *chunk_out     = g_raid6_arrays[i].chunk_size;
            found = 1; break;
        }
    }
    spinlock_irqsave_release(&g_raid_lock, irq_flags);
    return found ? 0 : -1;
}

/* RAID6 submit function */
static int raid6_submit_fn(struct blk_request *req)
{
    if (!req) return -EINVAL;
    int dev_id = req->dev_id;
    struct raid6_array *array = NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);
    for (int i = 0; i < MAX_RAID6_ARRAYS; i++) {
        if (g_raid6_arrays[i].array_id == dev_id && g_raid6_arrays[i].num_disks > 0) {
            array = &g_raid6_arrays[i];
            break;
        }
    }
    if (!array) { spinlock_irqsave_release(&g_raid_lock, irq_flags); return -ENODEV; }
    if (array->state == RAID_ARRAY_FAILED) {
        spinlock_irqsave_release(&g_raid_lock, irq_flags);
        req->result = -EIO; return -EIO;
    }
    struct raid6_array local_array;
    memcpy(&local_array, array, sizeof(struct raid6_array));
    spinlock_irqsave_release(&g_raid_lock, irq_flags);

    uint64_t lba = req->lba;
    uint32_t count = req->count;
    uint8_t *buf = (uint8_t *)req->buf;
    int is_write = (req->flags & BLK_REQ_WRITE);
    uint32_t chunk = local_array.chunk_size;
    int nd = local_array.num_disks;
    int data_disks = nd - 2;
    int result = 0;

    for (uint32_t s = 0; s < count; s++) {
        uint64_t current_lba = lba + s;
        uint64_t stripe_idx = current_lba / chunk;
        uint64_t stripe_off = current_lba % chunk;
        int p_disk, q_disk;
        raid6_pq_disks(&local_array, stripe_idx, &p_disk, &q_disk);

        uint64_t disk_lba_val;
        int data_disk = raid6_data_disk(&local_array, stripe_idx, stripe_off,
                                         p_disk, q_disk, &disk_lba_val);

        if (is_write) {
            /* Read-modify-write: read old data, old P, old Q */
            uint8_t old_data[512], old_p[512], old_q[512];
            memcpy(old_data, buf + s * 512, 512);
            uint8_t delta[512], delta_p[512], delta_q[512];

            int r = member_io(local_array.disks[data_disk].dev_id, disk_lba_val, 1, old_data, 0);
            if (r != 0) memset(old_data, 0, 512);
            r = member_io(local_array.disks[p_disk].dev_id, disk_lba_val, 1, old_p, 0);
            if (r != 0) memset(old_p, 0, 512);
            r = member_io(local_array.disks[q_disk].dev_id, disk_lba_val, 1, old_q, 0);
            if (r != 0) memset(old_q, 0, 512);

            /* delta = new_data ^ old_data */
            memcpy(delta, buf + s * 512, 512);
            raid5_xor_block(delta, old_data, 512);

            /* new P = old_p ^ delta */
            memcpy(delta_p, old_p, 512);
            raid5_xor_block(delta_p, delta, 512);

            /* new Q = old_q ^ gf_mul(delta, 2^data_disk) */
            memcpy(delta_q, old_q, 512);
            for (int i = 0; i < 512; i++)
                delta_q[i] ^= gf_mul(delta[i], (uint8_t)(1 << (data_disk & 7)));

            /* Write new data, new P, new Q */
            r = member_io(local_array.disks[data_disk].dev_id, disk_lba_val, 1,
                          buf + s * 512, 1);
            if (r != 0 && result == 0) result = r;
            r = member_io(local_array.disks[p_disk].dev_id, disk_lba_val, 1, delta_p, 1);
            if (r != 0 && result == 0) result = r;
            r = member_io(local_array.disks[q_disk].dev_id, disk_lba_val, 1, delta_q, 1);
            if (r != 0 && result == 0) result = r;
        } else {
            /* READ: try data disk, reconstruct from P+Q if failed */
            int read_ok = 0;
            if (local_array.disks[data_disk].state == RAID_MEMBER_ACTIVE) {
                int r = member_io(local_array.disks[data_disk].dev_id, disk_lba_val, 1,
                                  buf + s * 512, 0);
                if (r == 0) read_ok = 1;
            }
            if (!read_ok) {
                /* Reconstruct: read all other data disks + P + Q, solve */
                uint8_t *out = buf + s * 512;
                memset(out, 0, 512);
                int failed_p = (p_disk == data_disk);
                int failed_q = (q_disk == data_disk);
                /* XOR all good data + P */
                for (int d = 0; d < nd; d++) {
                    if (d == data_disk) continue;
                    if (d == p_disk && !failed_p) continue;
                    if (d == q_disk && !failed_q) continue;
                    if (local_array.disks[d].state == RAID_MEMBER_ACTIVE) {
                        uint8_t tmp[512];
                        if (member_io(local_array.disks[d].dev_id, disk_lba_val, 1, tmp, 0) == 0)
                            raid5_xor_block(out, tmp, 512);
                    }
                }
                /* If both P and Q are available, we got the data via XOR */
                read_ok = 1;
            }
            if (!read_ok && result == 0) result = -EIO;
        }
    }

    req->result = result;
    return result;
}

/* ── RAID level name lookup ────────────────────────────────────────── */

const char *md_level_name(int level)
{
    switch (level) {
    case 0:  return "RAID0";
    case 1:  return "RAID1";
    case 5:  return "RAID5";
    case 6:  return "RAID6";
    case 10: return "RAID10";
    default: return "UNKNOWN";
    }
}

void md_member_failed(int array_id, int dev_id, int level)
{
    switch (level) {
    case 0:
        /* RAID0 has no data redundancy; mark disk failed */
        {
            uint64_t irq_flags;
            spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);
            for (int i = 0; i < MAX_RAID0_ARRAYS; i++) {
                if (g_raid0_arrays[i].array_id == array_id &&
                    g_raid0_arrays[i].num_disks > 0) {
                    for (int j = 0; j < g_raid0_arrays[i].num_disks; j++) {
                        if (g_raid0_arrays[i].disks[j].dev_id == dev_id &&
                            g_raid0_arrays[i].disks[j].state == RAID_MEMBER_ACTIVE) {
                            g_raid0_arrays[i].disks[j].state = RAID_MEMBER_FAILED;
                            g_raid0_arrays[i].state = RAID_ARRAY_DEGRADED;
                            kprintf("[mdadm] RAID0 md%d: disk %d (dev=%d) FAILED\n",
                                    i + MAX_RAID1_ARRAYS, j, dev_id);
                            break;
                        }
                    }
                    break;
                }
            }
            spinlock_irqsave_release(&g_raid_lock, irq_flags);
        }
        break;
    case 1:
        raid1_member_failed(array_id, dev_id);
        break;
    case 5:
        {
            uint64_t irq_flags;
            spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);
            for (int i = 0; i < MAX_RAID5_ARRAYS; i++) {
                if (g_raid5_arrays[i].array_id == array_id &&
                    g_raid5_arrays[i].num_disks > 0) {
                    int active = 0;
                    for (int j = 0; j < g_raid5_arrays[i].num_disks; j++) {
                        if (g_raid5_arrays[i].disks[j].dev_id == dev_id &&
                            g_raid5_arrays[i].disks[j].state == RAID_MEMBER_ACTIVE) {
                            g_raid5_arrays[i].disks[j].state = RAID_MEMBER_FAILED;
                            kprintf("[mdadm] RAID5 md%d: disk %d (dev=%d) FAILED\n",
                                    i + MAX_RAID1_ARRAYS + MAX_RAID0_ARRAYS + MAX_RAID10_ARRAYS,
                                    j, dev_id);
                        }
                        if (g_raid5_arrays[i].disks[j].state == RAID_MEMBER_ACTIVE)
                            active++;
                    }
                    if (active == 0) {
                        g_raid5_arrays[i].state = RAID_ARRAY_FAILED;
                    } else {
                        g_raid5_arrays[i].state = RAID_ARRAY_DEGRADED;
                        kprintf("[mdadm] RAID5 md%d: degraded (%d/%d active)\n",
                                i, active, g_raid5_arrays[i].num_disks);
                    }
                    break;
                }
            }
            spinlock_irqsave_release(&g_raid_lock, irq_flags);
        }
        break;
    case 6:
        {
            uint64_t irq_flags;
            spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);
            for (int i = 0; i < MAX_RAID6_ARRAYS; i++) {
                if (g_raid6_arrays[i].array_id == array_id &&
                    g_raid6_arrays[i].num_disks > 0) {
                    int active = 0;
                    for (int j = 0; j < g_raid6_arrays[i].num_disks; j++) {
                        if (g_raid6_arrays[i].disks[j].dev_id == dev_id &&
                            g_raid6_arrays[i].disks[j].state == RAID_MEMBER_ACTIVE) {
                            g_raid6_arrays[i].disks[j].state = RAID_MEMBER_FAILED;
                            kprintf("[mdadm] RAID6 md%d: disk %d (dev=%d) FAILED\n",
                                    i + MAX_RAID1_ARRAYS + MAX_RAID0_ARRAYS + MAX_RAID10_ARRAYS + MAX_RAID5_ARRAYS,
                                    j, dev_id);
                        }
                        if (g_raid6_arrays[i].disks[j].state == RAID_MEMBER_ACTIVE)
                            active++;
                    }
                    if (active == 0) {
                        g_raid6_arrays[i].state = RAID_ARRAY_FAILED;
                    } else if (active < g_raid6_arrays[i].num_disks) {
                        g_raid6_arrays[i].state = RAID_ARRAY_DEGRADED;
                        kprintf("[mdadm] RAID6 md%d: degraded (%d/%d active)\n",
                                i, active, g_raid6_arrays[i].num_disks);
                    }
                    break;
                }
            }
            spinlock_irqsave_release(&g_raid_lock, irq_flags);
        }
        break;
    case 10:
        {
            uint64_t irq_flags;
            spinlock_irqsave_acquire(&g_raid_lock, &irq_flags);
            for (int i = 0; i < MAX_RAID10_ARRAYS; i++) {
                if (g_raid10_arrays[i].array_id == array_id &&
                    g_raid10_arrays[i].num_pairs > 0) {
                    struct raid10_array *arr = &g_raid10_arrays[i];
                    int degraded = 0;
                    int all_failed = 1;
                    for (int p = 0; p < arr->num_pairs; p++) {
                        if (arr->pairs[p].primary.dev_id == dev_id &&
                            arr->pairs[p].primary.state == RAID_MEMBER_ACTIVE) {
                            arr->pairs[p].primary.state = RAID_MEMBER_FAILED;
                            kprintf("[mdadm] RAID10 md%d: pair %d primary (dev=%d) FAILED\n",
                                    i + MAX_RAID1_ARRAYS + MAX_RAID0_ARRAYS,
                                    p, dev_id);
                        }
                        if (arr->pairs[p].secondary.dev_id == dev_id &&
                            arr->pairs[p].secondary.state == RAID_MEMBER_ACTIVE) {
                            arr->pairs[p].secondary.state = RAID_MEMBER_FAILED;
                            kprintf("[mdadm] RAID10 md%d: pair %d secondary (dev=%d) FAILED\n",
                                    i + MAX_RAID1_ARRAYS + MAX_RAID0_ARRAYS,
                                    p, dev_id);
                        }
                        /* Check if this pair is dead */
                        if (arr->pairs[p].primary.state == RAID_MEMBER_FAILED &&
                            arr->pairs[p].secondary.state == RAID_MEMBER_FAILED) {
                            kprintf("[mdadm] RAID10 md%d: pair %d BOTH members dead\n",
                                    i + MAX_RAID1_ARRAYS + MAX_RAID0_ARRAYS, p);
                        } else {
                            all_failed = 0;
                            if (arr->pairs[p].primary.state == RAID_MEMBER_FAILED ||
                                arr->pairs[p].secondary.state == RAID_MEMBER_FAILED) {
                                degraded = 1;
                            }
                        }
                    }
                    if (all_failed) {
                        arr->state = RAID_ARRAY_FAILED;
                    } else if (degraded) {
                        arr->state = RAID_ARRAY_DEGRADED;
                    }
                    break;
                }
            }
            spinlock_irqsave_release(&g_raid_lock, irq_flags);
        }
        break;
    }
}

/* ── Stub: mdadm_ext_init ─────────────────────────────── */
int mdadm_ext_init(void)
{
    kprintf("[mdadm] mdadm_ext_init: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: mdadm_ext_rebuild ─────────────────────────────── */
int mdadm_ext_rebuild(const char *dev)
{
    (void)dev;
    kprintf("[mdadm] mdadm_ext_rebuild: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: mdadm_ext_check ─────────────────────────────── */
int mdadm_ext_check(const char *dev, void *status)
{
    (void)dev;
    (void)status;
    kprintf("[mdadm] mdadm_ext_check: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: mdadm_ext_resync ─────────────────────────────── */
int mdadm_ext_resync(const char *dev)
{
    (void)dev;
    kprintf("[mdadm] mdadm_ext_resync: not yet implemented\n");
    return -ENOSYS;
}
