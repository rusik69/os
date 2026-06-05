/*
 * dm-zero.c — Device Mapper zero target
 *
 * Provides a block device that reads as zeros and discards writes.
 * Similar to Linux /dev/zero — useful for testing, benchmarking,
 * and as a foundation for sparse/scratch devices.
 *
 * Table format:
 *   start length zero
 *
 * Example:
 *   0 1048576 zero   — 512 MB zero device
 *   0 2097152 zero   — 1 GB zero device
 *
 * Item 325: dm-zero/dm-error test targets
 */

#define KERNEL_INTERNAL
#include "dm.h"
#include "string.h"
#include "printf.h"
#include "export.h"
#include "errno.h"

/* ── Target operations ────────────────────────────────────────────── */

/* Constructor: no arguments needed. */
static int zero_ctr(struct dm_target *ti, int argc, const char **argv)
{
    (void)argv;
    if (argc != 0) {
        kprintf("[dm-zero] ctr: zero target takes no arguments, got %d\n", argc);
        return -EINVAL;
    }
    ti->private = NULL;
    kprintf("[dm-zero] ctr: [%llu, %llu) zero target created\n",
            (unsigned long long)ti->start,
            (unsigned long long)(ti->start + ti->length));
    return 0;
}

/* Destructor: nothing to free. */
static void zero_dtr(struct dm_target *ti)
{
    (void)ti;
}

/* Map: handle read/write to zero device.
 * Reads: fill the buffer with zeros.
 * Writes: discard the data (no-op).
 * Completion is signalled via blk_request_done() and mapped_count is
 * set to 0 so the dm framework does not attempt to submit to a
 * backing device.
 */
static int zero_map(struct dm_target *ti, struct blk_request *req,
                    struct blk_request *mapped[], int *mapped_count)
{
    (void)ti;
    (void)mapped;

    if (req->flags & BLK_REQ_READ) {
        /* Zero out the entire request buffer (count in 512-byte sectors) */
        memset(req->buf, 0, (size_t)req->count * 512ULL);
    }
    /* Writes are silently discarded — no-op */

    req->result = 0;
    blk_request_done(req);

    /* No mapped requests to submit; we handled it entirely here */
    *mapped_count = 0;
    return 0;
}

/* ── Target operations struct ─────────────────────────────────────── */

static const struct dm_target_ops zero_ops = {
    .name  = "zero",
    .flags = 0,
    .ctr   = zero_ctr,
    .dtr   = zero_dtr,
    .map   = zero_map,
};

/* ── Init / registration ──────────────────────────────────────────── */

void dm_zero_init(void)
{
    int ret = dm_register_target(&zero_ops);
    if (ret == 0) {
        kprintf("[OK] dm-zero: zero target registered\n");
    } else {
        kprintf("[FAIL] dm-zero: registration failed: %d\n", ret);
    }
}
