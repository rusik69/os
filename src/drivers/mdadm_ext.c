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

/* Global table of RAID1 arrays */
static struct raid1_array g_raid1_arrays[MAX_RAID1_ARRAYS];
static spinlock_t g_raid1_lock;
static int g_raid1_next_id = 0; /* next MD block device id offset from MD_BLOCKDEV_BASE */
static int g_raid1_initialized = 0;

/* ── Forward declarations ──────────────────────────────────────────── */
static int raid1_submit_fn(struct blk_request *req);

/* ── Initialization ────────────────────────────────────────────────── */

void raid1_init(void)
{
    if (g_raid1_initialized) return;
    memset(g_raid1_arrays, 0, sizeof(g_raid1_arrays));
    spinlock_init(&g_raid1_lock);
    g_raid1_next_id = 0;
    g_raid1_initialized = 1;
    kprintf("[mdadm] RAID1 subsystem initialized (max %d arrays)\n",
            MAX_RAID1_ARRAYS);
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

    if (!g_raid1_initialized)
        raid1_init();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid1_lock, &irq_flags);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_RAID1_ARRAYS; i++) {
        if (g_raid1_arrays[i].num_members == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_raid1_lock, irq_flags);
        kprintf("[mdadm] RAID1: no free array slots\n");
        return -1;
    }

    /* Compute the MD block device id */
    int md_id = MD_BLOCKDEV_BASE + g_raid1_next_id;
    g_raid1_next_id = (g_raid1_next_id + 1) % (BLOCKDEV_MAX_DEVICES - MD_BLOCKDEV_BASE);

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
            spinlock_irqsave_release(&g_raid1_lock, irq_flags);
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
        spinlock_irqsave_release(&g_raid1_lock, irq_flags);
        return -1;
    }

    spinlock_irqsave_release(&g_raid1_lock, irq_flags);

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
    spinlock_irqsave_acquire(&g_raid1_lock, &irq_flags);

    for (int i = 0; i < MAX_RAID1_ARRAYS; i++) {
        if (g_raid1_arrays[i].array_id == array_id && g_raid1_arrays[i].num_members > 0) {
            int md_id = g_raid1_arrays[i].array_id;
            blockdev_unregister(md_id);
            kprintf("[mdadm] RAID1 array md%d (id=%d) destroyed\n", i, md_id);
            memset(&g_raid1_arrays[i], 0, sizeof(struct raid1_array));
            break;
        }
    }

    spinlock_irqsave_release(&g_raid1_lock, irq_flags);
}

/* ── Mark a member as failed ───────────────────────────────────────── */

int raid1_member_failed(int array_id, int dev_id)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid1_lock, &irq_flags);

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

    spinlock_irqsave_release(&g_raid1_lock, irq_flags);
    return found ? 0 : -1;
}

/* ── Query array status ────────────────────────────────────────────── */

int raid1_status(int array_id, int *state_out, int *num_members_out,
                 uint64_t *sectors_out)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_raid1_lock, &irq_flags);

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

    spinlock_irqsave_release(&g_raid1_lock, irq_flags);
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
    spinlock_irqsave_acquire(&g_raid1_lock, &irq_flags);

    for (int i = 0; i < MAX_RAID1_ARRAYS; i++) {
        if (g_raid1_arrays[i].array_id == dev_id && g_raid1_arrays[i].num_members > 0) {
            array = &g_raid1_arrays[i];
            break;
        }
    }

    if (!array) {
        spinlock_irqsave_release(&g_raid1_lock, irq_flags);
        return -ENODEV;
    }

    /* Check array state */
    if (array->state == RAID_ARRAY_FAILED) {
        spinlock_irqsave_release(&g_raid1_lock, irq_flags);
        req->result = -EIO;
        return -EIO;
    }

    /* Save a copy of the array state to work with outside the lock */
    struct raid1_array local_array;
    memcpy(&local_array, array, sizeof(struct raid1_array));
    spinlock_irqsave_release(&g_raid1_lock, irq_flags);

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
            spinlock_irqsave_acquire(&g_raid1_lock, &lock_flags);
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
            spinlock_irqsave_release(&g_raid1_lock, lock_flags);
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
