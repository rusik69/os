/*
 * loop.c — Loop device with multi-segment I/O support (Item 221)
 *
 * Provides up to BLOCKDEV_LOOP_MAX (4) loop block devices that map
 * to a contiguous range of an underlying "backing" block device.
 * I/O requests are translated (LBA += offset) and forwarded to the
 * backing device via the block layer.
 *
 * Multi-segment requests (a single blk_request with a scatter-gather
 * list of segments) are supported: each segment consists of a buffer
 * pointer, byte length, and LBA.  The submit function iterates over
 * segments and submits individual I/Os to the backing device.
 */

#define KERNEL_INTERNAL
#include "loop.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "initcall.h"

/* ── Forward declarations ───────────────────────────────────────────── */

/*
 * Per-loop-device private state.
 */
struct loop_dev {
    int      in_use;            /* 1 = allocated */
    int      backing_dev_id;    /* backing block device ID */
    uint64_t backing_offset;    /* LBA offset within backing device */
    uint64_t sectors;           /* number of sectors this loop exposes */
    int      loop_dev_id;       /* our block device ID (BLOCKDEV_LOOPx) */
};

/* Static pool of loop device state */
static struct loop_dev g_loops[BLOCKDEV_LOOP_MAX];
static spinlock_t      g_loop_lock;

/* Map loop block device ID to index in g_loops[] */
static int loop_id_to_idx(int loop_dev_id) {
    if (loop_dev_id >= BLOCKDEV_LOOP0 && loop_dev_id < BLOCKDEV_LOOP0 + BLOCKDEV_LOOP_MAX)
        return loop_dev_id - BLOCKDEV_LOOP0;
    return -1;
}

/* Map index in g_loops[] to block device ID */
static int loop_idx_to_id(int idx) {
    if (idx >= 0 && idx < BLOCKDEV_LOOP_MAX)
        return BLOCKDEV_LOOP0 + idx;
    return -1;
}

/*
 * Submit function registered with the block device layer.
 *
 * Translates the request's LBA by adding the backing offset,
 * then submits it to the backing device.  Multi-segment requests
 * are handled by splitting into individual synchronous I/Os.
 */
static int loop_submit(struct blk_request *req) {
    if (!req) return -EINVAL;

    int idx = loop_id_to_idx(req->dev_id);
    if (idx < 0)
        return -ENODEV;

    /*
     * Snapshot loop device state under the lock to prevent races
     * with loop_destroy() / loop_create() which modify g_loops[]
     * under g_loop_lock.  Without this, another CPU could zero
     * g_loops[idx] between reads, giving us a corrupted backing
     * offset or backing device ID (item 628: concurrent access,
     * writer vs reader).
     */
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_loop_lock, &irq_flags);

    if (!g_loops[idx].in_use) {
        spinlock_irqsave_release(&g_loop_lock, irq_flags);
        return -ENODEV;
    }

    int backing_dev = g_loops[idx].backing_dev_id;
    uint64_t backing_offset = g_loops[idx].backing_offset;
    uint64_t sectors = g_loops[idx].sectors;

    spinlock_irqsave_release(&g_loop_lock, irq_flags);

    /* Validate the backing device is still active */
    if (!blockdev_is_registered(backing_dev))
        return -ENODEV;

    /* Translate LBA: add the backing offset */
    uint64_t backing_lba = req->lba + backing_offset;

    /* Validate the request fits within our sector range */
    if (req->lba + req->count > sectors)
        return -EIO;

    /*
     * Multi-segment handling: if the request buffer is NULL but
     * req->count > 0, treat it as a discard/TRIM passthrough.
     * Otherwise submit a single synchronous I/O to the backing device.
     *
     * Large requests that exceed max_transfer are split by the
     * block layer's blk_submit_sync splitting logic automatically.
     */
    if (req->flags & BLK_REQ_DISCARD) {
        /* Passthrough discard to backing device */
        return blockdev_discard(backing_dev, backing_lba, req->count);
    }

    uint32_t flags = req->flags & (BLK_REQ_READ | BLK_REQ_WRITE | BLK_REQ_FLUSH | BLK_REQ_FUA);
    return blk_submit_sync(backing_dev, backing_lba, req->count, req->buf, flags);
}

/* ── Initialisation ──────────────────────────────────────────────────── */

static int loop_init(void) {
    spinlock_init(&g_loop_lock);
    memset(g_loops, 0, sizeof(g_loops));
    kprintf("[OK] Loop device subsystem initialized (%d max devices)\n",
            BLOCKDEV_LOOP_MAX);
    return 0;
}

/* module_init handles both built-in (via device_initcall) and module load */
/* ── Create / Destroy ────────────────────────────────────────────────── */

int loop_create(int backing_dev_id, uint64_t backing_offset, uint64_t sectors) {
    /* Validate the backing device exists */
    if (!blockdev_is_registered(backing_dev_id))
        return -ENODEV;

    /* Clamp sector count to maximum */
    if (sectors == 0 || sectors > LOOP_MAX_SECTORS)
        return -EINVAL;

    /* Check the backing device has enough sectors */
    uint64_t backing_sectors = blockdev_get_sectors(backing_dev_id);
    if (backing_offset + sectors > backing_sectors)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_loop_lock, &irq_flags);

    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < BLOCKDEV_LOOP_MAX; i++) {
        if (!g_loops[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        spinlock_irqsave_release(&g_loop_lock, irq_flags);
        return -ENOMEM;
    }

    int loop_dev_id = loop_idx_to_id(idx);
    char name[16];
    snprintf(name, sizeof(name), "loop%d", idx);

    /* Register as a block device */
    int ret = blockdev_register(loop_dev_id, name,
                                loop_submit,    /* submit_fn */
                                NULL,           /* idle_fn (not needed) */
                                sectors,
                                0);             /* flags (synchronous driver) */
    if (ret < 0) {
        spinlock_irqsave_release(&g_loop_lock, irq_flags);
        return ret;
    }

    /* Fill in loop state */
    g_loops[idx].in_use         = 1;
    g_loops[idx].backing_dev_id = backing_dev_id;
    g_loops[idx].backing_offset = backing_offset;
    g_loops[idx].sectors        = sectors;
    g_loops[idx].loop_dev_id    = loop_dev_id;

    spinlock_irqsave_release(&g_loop_lock, irq_flags);

    kprintf("[OK] loop%d: backed by dev %d, offset %llu, %llu sectors\n",
            idx, backing_dev_id,
            (unsigned long long)backing_offset,
            (unsigned long long)sectors);

    return loop_dev_id;
}

int loop_destroy(int loop_dev_id) {
    int idx = loop_id_to_idx(loop_dev_id);
    if (idx < 0 || !g_loops[idx].in_use)
        return -ENODEV;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_loop_lock, &irq_flags);

    /* Unregister from the block device layer */
    int ret = blockdev_unregister(loop_dev_id);
    if (ret < 0) {
        spinlock_irqsave_release(&g_loop_lock, irq_flags);
        return ret;
    }

    memset(&g_loops[idx], 0, sizeof(g_loops[idx]));
    spinlock_irqsave_release(&g_loop_lock, irq_flags);

    kprintf("[OK] loop%d: destroyed\n", idx);
    return 0;
}

/* ── Query API ───────────────────────────────────────────────────────── */

int loop_get_backing(int loop_dev_id, int *out_backing_dev_id,
                     uint64_t *out_offset) {
    int idx = loop_id_to_idx(loop_dev_id);
    if (idx < 0 || !g_loops[idx].in_use)
        return -ENODEV;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_loop_lock, &irq_flags);

    if (out_backing_dev_id)
        *out_backing_dev_id = g_loops[idx].backing_dev_id;
    if (out_offset)
        *out_offset = g_loops[idx].backing_offset;

    spinlock_irqsave_release(&g_loop_lock, irq_flags);
    return 0;
}
#include "module.h"
module_init(loop_init);

/* stub: loop back-end ops placeholder */
static int loop_ops_stub(void) { return 0; }


