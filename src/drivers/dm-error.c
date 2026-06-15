/*
 * dm-error.c — Device Mapper error target
 *
 * A block device that returns -EIO on all I/O operations.
 * Useful for testing error handling paths in filesystems,
 * block layer, and device mapper itself.
 *
 * Table format:
 *   start length error
 *
 * Example:
 *   0 1048576 error   — 512 MB device that fails all I/O
 *   0 2097152 error   — 1 GB error device
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
static int error_ctr(struct dm_target *ti, int argc, const char **argv)
{
    (void)argv;
    if (argc != 0) {
        kprintf("[dm-error] ctr: error target takes no arguments, got %d\n", argc);
        return -EINVAL;
    }
    ti->private = NULL;
    kprintf("[dm-error] ctr: [%llu, %llu) error target created\n",
            (unsigned long long)ti->start,
            (unsigned long long)(ti->start + ti->length));
    return 0;
}

/* Destructor: nothing to free. */
static void error_dtr(struct dm_target *ti)
{
    (void)ti;
}

/* Map: return -EIO for all I/O operations.
 * Completes the request with an error and sets mapped_count to 0
 * so the dm framework does not attempt to submit to a backing device.
 */
static int error_map(struct dm_target *ti, struct blk_request *req,
                     struct blk_request *mapped[], int *mapped_count)
{
    (void)ti;
    (void)mapped;

    /* Return I/O error — simulate a failed disk sector */
    req->result = -EIO;
    blk_request_done(req);

    /* No mapped requests to submit; we handled it entirely here */
    *mapped_count = 0;
    return 0;
}

/* ── Target operations struct ─────────────────────────────────────── */

static const struct dm_target_ops error_ops = {
    .name  = "error",
    .flags = 0,
    .ctr   = error_ctr,
    .dtr   = error_dtr,
    .map   = error_map,
};

/* ── Init / registration ──────────────────────────────────────────── */

void dm_error_init(void)
{
    int ret = dm_register_target(&error_ops);
    if (ret == 0) {
        kprintf("[OK] dm-error: error target registered\n");
    } else {
        kprintf("[FAIL] dm-error: registration failed: %d\n", ret);
    }
}
#include "module.h"
module_init(dm_error_init);
