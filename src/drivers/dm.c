/*
 * dm.c — Device Mapper framework
 *
 * Implements the core device mapper infrastructure:
 *   - Virtual block device creation/removal
 *   - Target type registration
 *   - Mapping table load/clear
 *   - I/O request mapping through targets
 *   - Suspend/resume with request queuing
 *
 * Item 321: Device mapper framework
 * Item 322: dm-linear target (in dm-linear.c)
 */

#define KERNEL_INTERNAL
#include "dm.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "export.h"
#include "errno.h"
#include "timer.h"

/* ── Global state ─────────────────────────────────────────────────── */

/* Device mapper device table */
static struct dm_device g_dm_devices[DM_MAX_DEVICES];
static spinlock_t g_dm_lock;

/* Target type registry */
static struct dm_target_type g_target_types[DM_MAX_TARGET_TYPES];
static int g_num_target_types = 0;

/* Initialised flag */
static int g_dm_initialised = 0;

/* ── Forward declarations ─────────────────────────────────────────── */

static int dm_submit_to_targets(struct dm_device *dm, struct blk_request *req);

/* ── Target type registry ─────────────────────────────────────────── */

int dm_register_target(const struct dm_target_ops *ops)
{
    if (!g_dm_initialised || !ops || !ops->name || !ops->ctr || !ops->dtr || !ops->map)
        return -EINVAL;

    spinlock_acquire(&g_dm_lock);
    if (g_num_target_types >= DM_MAX_TARGET_TYPES) {
        spinlock_release(&g_dm_lock);
        return -ENOSPC;
    }

    /* Check for duplicate name */
    for (int i = 0; i < g_num_target_types; i++) {
        if (strcmp(g_target_types[i].name, ops->name) == 0) {
            spinlock_release(&g_dm_lock);
            return -EEXIST;
        }
    }

    strncpy(g_target_types[g_num_target_types].name, ops->name,
            DM_TARGET_NAME_MAX - 1);
    g_target_types[g_num_target_types].name[DM_TARGET_NAME_MAX - 1] = '\0';
    g_target_types[g_num_target_types].ops = ops;
    g_target_types[g_num_target_types].registered = 1;
    g_num_target_types++;

    spinlock_release(&g_dm_lock);
    kprintf("[DM] Registered target type '%s' (flags=0x%x)\n",
            ops->name, (unsigned int)ops->flags);
    return 0;
}
EXPORT_SYMBOL(dm_register_target);

int dm_unregister_target(const struct dm_target_ops *ops)
{
    if (!g_dm_initialised || !ops || !ops->name)
        return -EINVAL;

    spinlock_acquire(&g_dm_lock);
    for (int i = 0; i < g_num_target_types; i++) {
        if (g_target_types[i].ops == ops) {
            g_target_types[i].registered = 0;
            g_target_types[i].ops = NULL;
            /* Compact the table */
            for (int j = i; j < g_num_target_types - 1; j++)
                g_target_types[j] = g_target_types[j + 1];
            g_num_target_types--;
            spinlock_release(&g_dm_lock);
            kprintf("[DM] Unregistered target type '%s'\n", ops->name);
            return 0;
        }
    }
    spinlock_release(&g_dm_lock);
    return -ENOENT;
}
EXPORT_SYMBOL(dm_unregister_target);

const struct dm_target_ops *dm_find_target(const char *name)
{
    if (!g_dm_initialised || !name)
        return NULL;

    spinlock_acquire(&g_dm_lock);
    for (int i = 0; i < g_num_target_types; i++) {
        if (g_target_types[i].registered &&
            strcmp(g_target_types[i].name, name) == 0) {
            const struct dm_target_ops *ops = g_target_types[i].ops;
            spinlock_release(&g_dm_lock);
            return ops;
        }
    }
    spinlock_release(&g_dm_lock);
    return NULL;
}
EXPORT_SYMBOL(dm_find_target);

/* ── Block device submit callback ─────────────────────────────────── */

/* Block driver submit_fn for device mapper virtual devices.
 * Called by the block layer when a request is submitted to a dm device. */
int dm_submit_request(struct blk_request *req)
{
    if (!req) return -EINVAL;

    int dm_id = req->dev_id - DM_DEVBASE;
    if (dm_id < 0 || dm_id >= DM_MAX_DEVICES)
        return -ENODEV;

    struct dm_device *dm = &g_dm_devices[dm_id];
    if (!dm->active) return -ENODEV;

    spinlock_acquire(&dm->lock);

    /* If suspended, queue the request for later processing */
    if (dm->suspended) {
        req->next = NULL;
        if (dm->suspended_tail) {
            dm->suspended_tail->next = req;
            dm->suspended_tail = req;
        } else {
            dm->suspended_head = req;
            dm->suspended_tail = req;
        }
        dm->suspended_count++;
        spinlock_release(&dm->lock);
        return 0;
    }

    spinlock_release(&dm->lock);

    /* Route through target mapping */
    return dm_submit_to_targets(dm, req);
}

/* Route a request through the target chain.
 * Iterates targets that overlap the request's sector range. */
static int dm_submit_to_targets(struct dm_device *dm, struct blk_request *req)
{
    uint64_t req_start = req->lba;
    uint64_t req_end   = req->lba + req->count;  /* exclusive */
    int ret = 0;

    spinlock_acquire(&dm->lock);

    struct dm_target *ti;
    list_for_each_entry(ti, &dm->targets, list) {
        uint64_t ti_end = ti->start + ti->length;  /* exclusive */

        /* Check if this target overlaps the request */
        if (req_end <= ti->start || req_start >= ti_end)
            continue;

        /* Clip the request to this target's range */
        uint64_t overlap_start = req_start > ti->start ? req_start : ti->start;
        uint64_t overlap_end   = req_end < ti_end ? req_end : ti_end;
        uint64_t overlap_count = overlap_end - overlap_start;

        if (overlap_count == 0)
            continue;

        /* Build a sub-request for this target's portion.
         * For now, we handle single-target devices efficiently by
         * calling map() directly with the original request.
         * Multi-target devices clone the request. */
        if (dm->num_targets == 1 && ti->start == 0 &&
            ti->length >= req_start + req->count) {
            /* Fast path: single target covers the entire device.
             * Let the target map the request directly. */
            struct blk_request *mapped[4];
            int mapped_count = 0;

            spinlock_release(&dm->lock);
            ret = ti->ops->map(ti, req, mapped, &mapped_count);
            if (ret != 0) return ret;

            /* Submit each mapped request synchronously via block layer */
            for (int i = 0; i < mapped_count; i++) {
                int sub_ret = blk_submit_sync(mapped[i]->dev_id,
                                               mapped[i]->lba,
                                               mapped[i]->count,
                                               mapped[i]->buf,
                                               mapped[i]->flags);
                if (sub_ret != 0) {
                    ret = sub_ret;
                    break;
                }
            }
            return ret;
        }

        /* Multi-target or partial coverage: clone the request range
         * and map each piece separately.
         * Note: This is a simplified implementation. A production dm
         * would use a more sophisticated bio splitting approach. */
        struct blk_request *sub_req = blk_request_alloc();
        if (!sub_req) return -ENOMEM;

        uint64_t offset_in_req = overlap_start - req_start;
        uint64_t offset_bytes  = offset_in_req * 512;
        sub_req->lba    = overlap_start;
        sub_req->count  = (uint32_t)overlap_count;
        sub_req->flags  = req->flags;
        sub_req->buf    = (uint8_t *)req->buf + offset_bytes;
        sub_req->dev_id = req->dev_id;

        struct blk_request *mapped[4];
        int mapped_count = 0;
        ret = ti->ops->map(ti, sub_req, mapped, &mapped_count);
        if (ret != 0) {
            blk_request_free(sub_req);
            spinlock_release(&dm->lock);
            return ret;
        }

        /* Submit each mapped sub-request */
        for (int i = 0; i < mapped_count; i++) {
            int sub_ret = blk_submit_sync(mapped[i]->dev_id,
                                           mapped[i]->lba,
                                           mapped[i]->count,
                                           mapped[i]->buf,
                                           mapped[i]->flags);
            if (sub_ret != 0) ret = sub_ret;
            blk_request_free(mapped[i]);
        }
        blk_request_free(sub_req);
    }

    spinlock_release(&dm->lock);
    return ret;
}

/* ── Device creation / removal ────────────────────────────────────── */

int dm_device_create(const char *name, uint64_t size)
{
    if (!g_dm_initialised || !name || !*name)
        return -EINVAL;

    if (size == 0) return -EINVAL;

    if (strlen(name) >= DM_NAME_MAX)
        return -ENAMETOOLONG;

    spinlock_acquire(&g_dm_lock);

    /* Check for duplicate name */
    for (int i = 0; i < DM_MAX_DEVICES; i++) {
        if (g_dm_devices[i].active &&
            strcmp(g_dm_devices[i].name, name) == 0) {
            spinlock_release(&g_dm_lock);
            return -EEXIST;
        }
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < DM_MAX_DEVICES; i++) {
        if (!g_dm_devices[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_release(&g_dm_lock);
        return -ENOSPC;
    }

    /* Block device ID */
    int dev_id = DM_DEVBASE + slot;

    /* Check blockdev slot is available */
    if (blockdev_is_registered(dev_id)) {
        spinlock_release(&g_dm_lock);
        return -EBUSY;
    }

    /* Initialise the dm device struct */
    struct dm_device *dm = &g_dm_devices[slot];
    memset(dm, 0, sizeof(*dm));
    strncpy(dm->name, name, DM_NAME_MAX - 1);
    dm->name[DM_NAME_MAX - 1] = '\0';
    dm->dev_id = dev_id;
    dm->sector_count = size;
    dm->active = 1;
    dm->suspended = 0;
    INIT_LIST_HEAD(&dm->targets);
    dm->num_targets = 0;
    spinlock_init(&dm->lock);
    dm->suspended_head = NULL;
    dm->suspended_tail = NULL;
    dm->suspended_count = 0;

    /* Register as a block device */
    int ret = blockdev_register(dev_id, name,
                                dm_submit_request,
                                NULL,  /* no idle fn needed */
                                size,
                                0);    /* synchronous driver */
    if (ret != 0) {
        dm->active = 0;
        spinlock_release(&g_dm_lock);
        kprintf("[DM] Failed to register block device for '%s': %d\n",
                name, ret);
        return ret;
    }

    spinlock_release(&g_dm_lock);
    kprintf("[DM] Created device '%s' (dm-%d, %llu sectors)\n",
            name, slot, (unsigned long long)size);
    return slot;
}
EXPORT_SYMBOL(dm_device_create);

int dm_device_remove(int dm_id)
{
    if (dm_id < 0 || dm_id >= DM_MAX_DEVICES)
        return -ENODEV;

    struct dm_device *dm = &g_dm_devices[dm_id];

    spinlock_acquire(&dm->lock);
    if (!dm->active) {
        spinlock_release(&dm->lock);
        return -ENODEV;
    }

    /* Destroy all targets */
    struct dm_target *ti, *tmp;
    list_for_each_entry_safe(ti, tmp, &dm->targets, list) {
        list_del(&ti->list);
        if (ti->ops && ti->ops->dtr)
            ti->ops->dtr(ti);
        kfree(ti);
    }
    dm->num_targets = 0;

    /* Drain any suspended requests (complete with error) */
    struct blk_request *req = dm->suspended_head;
    while (req) {
        struct blk_request *next = req->next;
        req->result = -EIO;
        req->done = 1;
        if (req->done_wq)
            wait_queue_wake_all(req->done_wq);
        req = next;
    }
    dm->suspended_head = NULL;
    dm->suspended_tail = NULL;
    dm->suspended_count = 0;

    spinlock_release(&dm->lock);

    /* Unregister the block device */
    blockdev_unregister(dm->dev_id);

    dm->active = 0;
    kprintf("[DM] Removed device 'dm-%d' (%s)\n", dm_id, dm->name);
    return 0;
}
EXPORT_SYMBOL(dm_device_remove);

/* ── Suspend / Resume ─────────────────────────────────────────────── */

int dm_device_suspend(int dm_id)
{
    if (dm_id < 0 || dm_id >= DM_MAX_DEVICES)
        return -ENODEV;

    struct dm_device *dm = &g_dm_devices[dm_id];
    spinlock_acquire(&dm->lock);
    if (!dm->active) {
        spinlock_release(&dm->lock);
        return -ENODEV;
    }
    dm->suspended = 1;
    spinlock_release(&dm->lock);
    kprintf("[DM] Suspended device 'dm-%d' (%s)\n", dm_id, dm->name);
    return 0;
}
EXPORT_SYMBOL(dm_device_suspend);

int dm_device_resume(int dm_id)
{
    if (dm_id < 0 || dm_id >= DM_MAX_DEVICES)
        return -ENODEV;

    struct dm_device *dm = &g_dm_devices[dm_id];
    spinlock_acquire(&dm->lock);
    if (!dm->active) {
        spinlock_release(&dm->lock);
        return -ENODEV;
    }
    dm->suspended = 0;

    /* Drain the suspended queue */
    struct blk_request *head = dm->suspended_head;
    dm->suspended_head = NULL;
    dm->suspended_tail = NULL;
    int count = dm->suspended_count;
    dm->suspended_count = 0;
    spinlock_release(&dm->lock);

    /* Process all queued requests outside the lock */
    struct blk_request *req = head;
    while (req) {
        struct blk_request *next = req->next;
        req->next = NULL;
        dm_submit_to_targets(dm, req);
        req = next;
    }

    kprintf("[DM] Resumed device 'dm-%d' (%s), flushed %d queued I/O(s)\n",
            dm_id, dm->name, count);
    return 0;
}
EXPORT_SYMBOL(dm_device_resume);

/* ── Table management ─────────────────────────────────────────────── */

int dm_table_load(int dm_id, const char *table)
{
    if (dm_id < 0 || dm_id >= DM_MAX_DEVICES || !table)
        return -EINVAL;

    struct dm_device *dm = &g_dm_devices[dm_id];
    if (!dm->active) return -ENODEV;

    /* We'll build new targets in a temporary list first */
    struct list_head new_targets;
    INIT_LIST_HEAD(&new_targets);
    int new_count = 0;

    /* Parse the table string — one target per line:
     *   start_sectors length_sectors target_type args...
     * Lines are separated by '\n'.
     */
    const char *p = table;
    int line_num = 0;
    int ret = 0;

    while (*p) {
        /* Skip leading whitespace and blank lines */
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;

        line_num++;

        /* Parse: start length type [args...] */
        char start_str[32], len_str[32], type_str[DM_TARGET_NAME_MAX];
        char *args[8];
        int argc = 0;

        /* Read start sector (decimal) */
        int i = 0;
        while (*p >= '0' && *p <= '9' && i < (int)sizeof(start_str) - 1)
            start_str[i++] = *p++;
        start_str[i] = '\0';
        if (i == 0) { ret = -EINVAL; break; }
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) { ret = -EINVAL; break; }

        /* Read length (decimal) */
        i = 0;
        while (*p >= '0' && *p <= '9' && i < (int)sizeof(len_str) - 1)
            len_str[i++] = *p++;
        len_str[i] = '\0';
        if (i == 0) { ret = -EINVAL; break; }
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) { ret = -EINVAL; break; }

        /* Read target type name */
        i = 0;
        while (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\0' &&
               i < (int)sizeof(type_str) - 1)
            type_str[i++] = *p++;
        type_str[i] = '\0';
        if (i == 0) { ret = -EINVAL; break; }

        /* Parse remaining args (space-separated until end of line) */
        while (*p == ' ' || *p == '\t') p++;
        while (*p && *p != '\n' && argc < 8) {
            /* Collect one arg */
            static char argbufs[8][64];
            int ai = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
                   ai < (int)sizeof(argbufs[0]) - 1) {
                argbufs[argc][ai++] = *p++;
            }
            argbufs[argc][ai] = '\0';
            if (ai > 0) {
                args[argc] = argbufs[argc];
                argc++;
            }
            while (*p == ' ' || *p == '\t') p++;
        }

        /* Skip trailing newline */
        if (*p == '\n') p++;

        /* Convert numeric fields */
        uint64_t start = 0, length = 0;
        {
            const char *s = start_str;
            while (*s) { start = start * 10 + (uint64_t)(*s++ - '0'); }
        }
        {
            const char *s = len_str;
            while (*s) { length = length * 10 + (uint64_t)(*s++ - '0'); }
        }

        /* Validate range */
        if (start + length > dm->sector_count) {
            kprintf("[DM] Table line %d: range [%llu, %llu) exceeds device size %llu\n",
                    line_num, (unsigned long long)start,
                    (unsigned long long)(start + length),
                    (unsigned long long)dm->sector_count);
            ret = -EINVAL;
            break;
        }

        /* Find the target type */
        const struct dm_target_ops *ops = dm_find_target(type_str);
        if (!ops) {
            kprintf("[DM] Table line %d: unknown target type '%s'\n",
                    line_num, type_str);
            ret = -ENOENT;
            break;
        }

        /* Allocate target instance */
        struct dm_target *ti = (struct dm_target *)
            kmalloc(sizeof(struct dm_target));
        if (!ti) { ret = -ENOMEM; break; }
        memset(ti, 0, sizeof(*ti));
        ti->dm = dm;
        ti->ops = ops;
        ti->start = start;
        ti->length = length;
        ti->private = NULL;

        /* Construct the target */
        ret = ops->ctr(ti, argc, (const char **)args);
        if (ret != 0) {
            kprintf("[DM] Table line %d: target '%s' ctr failed: %d\n",
                    line_num, type_str, ret);
            kfree(ti);
            break;
        }

        /* Add to new target list (maintains sorted order) */
        list_add_tail(&ti->list, &new_targets);
        new_count++;
    }

    if (ret != 0) {
        /* Clean up partially built targets */
        struct dm_target *ti, *tmp;
        list_for_each_entry_safe(ti, tmp, &new_targets, list) {
            list_del(&ti->list);
            if (ti->ops && ti->ops->dtr) ti->ops->dtr(ti);
            kfree(ti);
        }
        return ret;
    }

    /* Replace the existing table atomically */
    spinlock_acquire(&dm->lock);

    /* Destroy old targets */
    struct dm_target *oti, *otmp;
    list_for_each_entry_safe(oti, otmp, &dm->targets, list) {
        list_del(&oti->list);
        if (oti->ops && oti->ops->dtr)
            oti->ops->dtr(oti);
        kfree(oti);
    }

    /* Install new targets */
    dm->targets = new_targets;
    dm->num_targets = new_count;

    spinlock_release(&dm->lock);

    kprintf("[DM] Loaded table for 'dm-%d' (%s): %d target(s)\n",
            dm_id, dm->name, new_count);
    return 0;
}
EXPORT_SYMBOL(dm_table_load);

int dm_table_clear(int dm_id)
{
    if (dm_id < 0 || dm_id >= DM_MAX_DEVICES)
        return -ENODEV;

    struct dm_device *dm = &g_dm_devices[dm_id];
    if (!dm->active) return -ENODEV;

    spinlock_acquire(&dm->lock);
    struct dm_target *ti, *tmp;
    list_for_each_entry_safe(ti, tmp, &dm->targets, list) {
        list_del(&ti->list);
        if (ti->ops && ti->ops->dtr)
            ti->ops->dtr(ti);
        kfree(ti);
    }
    dm->num_targets = 0;
    spinlock_release(&dm->lock);
    return 0;
}
EXPORT_SYMBOL(dm_table_clear);

int dm_table_target_count(int dm_id)
{
    if (dm_id < 0 || dm_id >= DM_MAX_DEVICES)
        return -ENODEV;

    struct dm_device *dm = &g_dm_devices[dm_id];
    if (!dm->active) return -ENODEV;

    return dm->num_targets;
}
EXPORT_SYMBOL(dm_table_target_count);

/* ── Status query ─────────────────────────────────────────────────── */

int dm_device_status(int dm_id, char *buf, int max)
{
    if (dm_id < 0 || dm_id >= DM_MAX_DEVICES || !buf || max <= 0)
        return -EINVAL;

    struct dm_device *dm = &g_dm_devices[dm_id];
    if (!dm->active) return -ENODEV;

    int pos = 0;
    pos += snprintf(buf + pos, max - pos > 0 ? (size_t)(max - pos) : 0,
                    "name: %s\n"
                    "dev_id: %d\n"
                    "sectors: %llu\n"
                    "targets: %d\n"
                    "suspended: %d\n"
                    "suspended_io: %d\n",
                    dm->name,
                    dm->dev_id,
                    (unsigned long long)dm->sector_count,
                    dm->num_targets,
                    dm->suspended,
                    dm->suspended_count);

    spinlock_acquire(&dm->lock);
    struct dm_target *ti;
    list_for_each_entry(ti, &dm->targets, list) {
        int remaining = max - pos;
        if (remaining <= 0) break;
        pos += snprintf(buf + pos, (size_t)(remaining > 0 ? remaining : 0),
                        "  target: [%llu, %llu) type=%s\n",
                        (unsigned long long)ti->start,
                        (unsigned long long)(ti->start + ti->length),
                        ti->ops ? ti->ops->name : "?");
    }
    spinlock_release(&dm->lock);

    return pos;
}
EXPORT_SYMBOL(dm_device_status);

/* ── Lookup helpers ───────────────────────────────────────────────── */

struct dm_device *dm_device_get(int dm_id)
{
    if (dm_id < 0 || dm_id >= DM_MAX_DEVICES)
        return NULL;
    struct dm_device *dm = &g_dm_devices[dm_id];
    return dm->active ? dm : NULL;
}
EXPORT_SYMBOL(dm_device_get);

struct dm_device *dm_device_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < DM_MAX_DEVICES; i++) {
        if (g_dm_devices[i].active &&
            strcmp(g_dm_devices[i].name, name) == 0) {
            return &g_dm_devices[i];
        }
    }
    return NULL;
}
EXPORT_SYMBOL(dm_device_find);

/* ── Initialisation ───────────────────────────────────────────────── */

void dm_init(void)
{
    memset(g_dm_devices, 0, sizeof(g_dm_devices));
    memset(g_target_types, 0, sizeof(g_target_types));
    g_num_target_types = 0;
    spinlock_init(&g_dm_lock);
    g_dm_initialised = 1;
    kprintf("[OK] Device mapper framework initialized (%d devices max, %d target types max)\n",
            DM_MAX_DEVICES, DM_MAX_TARGET_TYPES);
}
EXPORT_SYMBOL(dm_init);
#include "module.h"
module_init(dm_init);

/* ── Stub: dm_create ─────────────────────────────── */
int dm_create(const char *name, const char *target)
{
    (void)name;
    (void)target;
    kprintf("[dm] dm_create: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: dm_remove ─────────────────────────────── */
int dm_remove(const char *name)
{
    (void)name;
    kprintf("[dm] dm_remove: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: dm_suspend ─────────────────────────────── */
int dm_suspend(const char *name)
{
    (void)name;
    kprintf("[dm] dm_suspend: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: dm_resume ─────────────────────────────── */
int dm_resume(const char *name)
{
    (void)name;
    kprintf("[dm] dm_resume: not yet implemented\n");
    return -ENOSYS;
}
