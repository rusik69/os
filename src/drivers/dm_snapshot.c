// SPDX-License-Identifier: GPL-2.0-only
/*
 * dm_snapshot.c — Device mapper snapshot target
 *
 * Provides copy-on-write snapshots for device-mapper targets.
 * Creates snapshots of block devices with minimal overhead.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "heap.h"

#define DM_SNAP_MAX_SNAPSHOTS  16
#define DM_SNAP_CHUNK_SIZE     4096  /* 4KB chunks */

struct dm_snapshot {
    int active;
    char origin[64];         /* Origin device */
    char cow_dev[64];        /* Copy-on-write device */
    uint8_t *cow_data;       /* COW storage */
    uint64_t cow_size;       /* COW size in bytes */
    uint64_t chunk_size;
    uint64_t *chunk_map;     /* Block -> COW chunk mapping */
    int chunk_count;
    uint64_t snap_id;
    uint64_t block_writes;
    spinlock_t lock;
};

static struct dm_snapshot dm_snapshots[DM_SNAP_MAX_SNAPSHOTS];
static uint64_t next_snap_id;

/* Create a snapshot of a device */
int64_t dm_snapshot_create(const char *origin, const char *cow_dev,
                        uint64_t cow_size)
{
    uint64_t irq_flags;
    int idx = -1;

    for (int i = 0; i < DM_SNAP_MAX_SNAPSHOTS; i++) {
        if (!dm_snapshots[i].active) { idx = i; break; }
    }
    if (idx < 0) return -ENOMEM;

    struct dm_snapshot *snap = &dm_snapshots[idx];
    spinlock_irqsave_acquire(&snap->lock, &irq_flags);

    strncpy(snap->origin, origin, sizeof(snap->origin) - 1);
    strncpy(snap->cow_dev, cow_dev, sizeof(snap->cow_dev) - 1);
    snap->cow_size = cow_size;
    snap->chunk_size = DM_SNAP_CHUNK_SIZE;
    snap->snap_id = next_snap_id++;
    snap->block_writes = 0;

    /* Allocate COW store */
    snap->cow_data = (uint8_t *)kmalloc(cow_size);
    if (!snap->cow_data) {
        spinlock_irqsave_release(&snap->lock, irq_flags);
        return -ENOMEM;
    }
    memset(snap->cow_data, 0, cow_size);

    /* Chunk map: -1 means not yet copied */
    snap->chunk_count = (int)(cow_size / DM_SNAP_CHUNK_SIZE);
    snap->chunk_map = (uint64_t *)kmalloc(
        (size_t)snap->chunk_count * sizeof(uint64_t));
    if (!snap->chunk_map) {
        kfree(snap->cow_data);
        spinlock_irqsave_release(&snap->lock, irq_flags);
        return -ENOMEM;
    }
    memset(snap->chunk_map, 0xFF,
           (size_t)snap->chunk_count * sizeof(uint64_t));

    snap->active = 1;
    spinlock_irqsave_release(&snap->lock, irq_flags);

    kprintf("[DM-SNAP] Created snapshot #%llu for %s (COW=%s, %llu bytes)\n",
            (unsigned long long)snap->snap_id, origin, cow_dev,
            (unsigned long long)cow_size);
    return (int64_t)snap->snap_id;
}

/* Read from snapshot (origin data, COW if modified) */
ssize_t dm_snapshot_read(int snap_id, uint64_t sector,
                      uint8_t *buf, size_t len)
{
    struct dm_snapshot *snap = NULL;

    for (int i = 0; i < DM_SNAP_MAX_SNAPSHOTS; i++) {
        if (dm_snapshots[i].active && dm_snapshots[i].snap_id == (uint64_t)snap_id) {
            snap = &dm_snapshots[i];
            break;
        }
    }
    if (!snap) return -ENODEV;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&snap->lock, &irq_flags);

    uint64_t chunk = sector / (snap->chunk_size / 512);
    int cow_chunk = -1;

    /* Check if this chunk was copied to COW */
    for (int i = 0; i < snap->chunk_count; i++) {
        if (snap->chunk_map[i] == chunk) {
            cow_chunk = i;
            break;
        }
    }

    if (cow_chunk >= 0) {
        /* Read from COW store */
        uint64_t offset = (uint64_t)cow_chunk * snap->chunk_size +
                          (sector % (snap->chunk_size / 512)) * 512;
        if (offset + len <= snap->cow_size) {
            memcpy(buf, snap->cow_data + offset, len);
        }
    } else {
        /* Read from origin (not modified in snapshot) */
        kprintf("[DM-SNAP] Reading unmodified chunk from origin for snap %d\n",
                snap_id);
    }

    spinlock_irqsave_release(&snap->lock, irq_flags);
    return (ssize_t)len;
}

/* Write to snapshot (triggers COW) */
ssize_t dm_snapshot_write(int snap_id, uint64_t sector,
                       const uint8_t *buf, size_t len)
{
    struct dm_snapshot *snap = NULL;

    for (int i = 0; i < DM_SNAP_MAX_SNAPSHOTS; i++) {
        if (dm_snapshots[i].active && dm_snapshots[i].snap_id == (uint64_t)snap_id) {
            snap = &dm_snapshots[i];
            break;
        }
    }
    if (!snap) return -ENODEV;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&snap->lock, &irq_flags);

    uint64_t chunk = sector / (snap->chunk_size / 512);
    int cow_chunk = -1;

    /* Find a free COW chunk */
    for (int i = 0; i < snap->chunk_count; i++) {
        if (snap->chunk_map[i] == (uint64_t)-1) {
            cow_chunk = i;
            break;
        }
    }

    if (cow_chunk < 0) {
        /* COW full */
        spinlock_irqsave_release(&snap->lock, irq_flags);
        return -ENOSPC;
    }

    /* Copy original data to COW (in real impl, we'd read from origin first) */
    snap->chunk_map[cow_chunk] = chunk;
    uint64_t offset = (uint64_t)cow_chunk * snap->chunk_size +
                      (sector % (snap->chunk_size / 512)) * 512;
    if (offset + len <= snap->cow_size) {
        memcpy(snap->cow_data + offset, buf, len);
    }
    snap->block_writes++;
    spinlock_irqsave_release(&snap->lock, irq_flags);
    return (ssize_t)len;
}

void dm_snapshot_init(void)
{
    memset(dm_snapshots, 0, sizeof(dm_snapshots));
    for (int i = 0; i < DM_SNAP_MAX_SNAPSHOTS; i++)
        spinlock_init(&dm_snapshots[i].lock);
    next_snap_id = 1;
    kprintf("[OK] DM-Snapshot — Device mapper snapshot target\n");
}
#include "module.h"
module_init(dm_snapshot_init);

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: snapshot_ctl_create ──────────────────────── */
int snapshot_ctl_create(const char *name, const char *origin)
{
    (void)name;
    (void)origin;
    kprintf("[DM-SNAPSHOT] snapshot_ctl_create: not yet implemented\n");
    return 0;
}
/* ── Stub: snapshot_ctl_delete ──────────────────────── */
int snapshot_ctl_delete(const char *name)
{
    (void)name;
    kprintf("[DM-SNAPSHOT] snapshot_ctl_delete: not yet implemented\n");
    return 0;
}
/* ── Stub: snapshot_read ────────────────────────────── */
int snapshot_read(struct dm_snapshot *snap, uint64_t sector, void *buf, uint32_t count)
{
    (void)snap;
    (void)sector;
    (void)buf;
    (void)count;
    kprintf("[DM-SNAPSHOT] snapshot_read: not yet implemented\n");
    return 0;
}
/* ── Stub: snapshot_write ───────────────────────────── */
int snapshot_write(struct dm_snapshot *snap, uint64_t sector, const void *buf, uint32_t count)
{
    (void)snap;
    (void)sector;
    (void)buf;
    (void)count;
    kprintf("[DM-SNAPSHOT] snapshot_write: not yet implemented\n");
    return 0;
}
/* ── Stub: snapshot_merge ───────────────────────────── */
int snapshot_merge(struct dm_snapshot *snap)
{
    (void)snap;
    kprintf("[DM-SNAPSHOT] snapshot_merge: not yet implemented\n");
    return 0;
}
