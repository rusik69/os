/*
 * dm-linear.c — Device Mapper linear target
 *
 * Maps a contiguous range of virtual sectors to a contiguous range
 * on an underlying block device.  This is the simplest dm target and
 * serves as the foundation for LVM-style logical volumes.
 *
 * Table format:
 *   start length linear <backing_dev_id> <start_sector>
 *
 * Example:
 *   0 1048576 linear 0 0   — 512 MB from sda (dev 0) at sector 0
 *   0 2097152 linear 3 0   — 1 GB from loop0 (dev 3) at sector 0
 *
 * Item 322: dm-linear target
 */

#define KERNEL_INTERNAL
#include "dm.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "export.h"
#include "errno.h"

/* ── Private per-target data ──────────────────────────────────────── */

struct linear_private {
    int    backing_dev_id;   /* block device ID of the backing device */
    uint64_t start_sector;   /* first sector on the backing device */
};

/* ── Target operations ────────────────────────────────────────────── */

/* Constructor: parse "dev_id start_sector" arguments. */
static int linear_ctr(struct dm_target *ti, int argc, const char **argv)
{
    if (argc < 2) {
        kprintf("[dm-linear] ctr: need 2 args (dev_id start_sector), got %d\n", argc);
        return -EINVAL;
    }

    struct linear_private *priv = (struct linear_private *)
        kmalloc(sizeof(struct linear_private));
    if (!priv) return -ENOMEM;
    memset(priv, 0, sizeof(*priv));

    /* Parse backing device ID */
    int dev_id = 0;
    const char *s = argv[0];
    while (*s) {
        if (*s < '0' || *s > '9') {
            kprintf("[dm-linear] ctr: invalid dev_id '%s'\n", argv[0]);
            kfree(priv);
            return -EINVAL;
        }
        dev_id = dev_id * 10 + (*s++ - '0');
    }

    /* Validate the backing device exists */
    if (!blockdev_is_registered(dev_id)) {
        kprintf("[dm-linear] ctr: backing device %d not registered\n", dev_id);
        kfree(priv);
        return -ENODEV;
    }

    /* Parse start sector on backing device */
    uint64_t start = 0;
    s = argv[1];
    while (*s) {
        if (*s < '0' || *s > '9') {
            kprintf("[dm-linear] ctr: invalid start_sector '%s'\n", argv[1]);
            kfree(priv);
            return -EINVAL;
        }
        start = start * 10 + (*s++ - '0');
    }

    /* Check that the range fits on the backing device */
    uint64_t backing_sectors = blockdev_get_sectors(dev_id);
    if (start + ti->length > backing_sectors) {
        kprintf("[dm-linear] ctr: range [%llu, %llu) exceeds backing device size %llu\n",
                (unsigned long long)start,
                (unsigned long long)(start + ti->length),
                (unsigned long long)backing_sectors);
        kfree(priv);
        return -EINVAL;
    }

    priv->backing_dev_id = dev_id;
    priv->start_sector   = start;
    ti->private = priv;

    kprintf("[dm-linear] ctr: [%llu, %llu) -> dev %d sector %llu\n",
            (unsigned long long)ti->start,
            (unsigned long long)(ti->start + ti->length),
            dev_id, (unsigned long long)start);
    return 0;
}

/* Destructor: free private data. */
static void linear_dtr(struct dm_target *ti)
{
    if (ti->private) {
        kfree(ti->private);
        ti->private = NULL;
    }
}

/* Map: translate virtual LBA -> backing device LBA.
 * Linear mapping: result_lba = virtual_lba - ti->start + priv->start_sector
 */
static int linear_map(struct dm_target *ti, struct blk_request *req,
                      struct blk_request *mapped[], int *mapped_count)
{
    struct linear_private *priv = (struct linear_private *)ti->private;
    if (!priv) return -EINVAL;

    /* Translate the sector range */
    uint64_t offset = req->lba - ti->start;
    uint64_t target_lba = priv->start_sector + offset;

    /* Reuse the original request but adjust dev_id and lba.
     * The caller (dm_submit_request) will submit this to the block layer. */
    req->dev_id = priv->backing_dev_id;
    req->lba    = target_lba;

    mapped[0] = req;
    *mapped_count = 1;
    return 0;
}

/* ── Target operations struct ─────────────────────────────────────── */

static const struct dm_target_ops linear_ops = {
    .name  = "linear",
    .flags = DM_TARGET_PASSES_THROUGH,
    .ctr   = linear_ctr,
    .dtr   = linear_dtr,
    .map   = linear_map,
};

/* ── Init / registration ──────────────────────────────────────────── */

void dm_linear_init(void)
{
    int ret = dm_register_target(&linear_ops);
    if (ret == 0) {
        kprintf("[OK] dm-linear: linear target registered\n");
    } else {
        kprintf("[FAIL] dm-linear: registration failed: %d\n", ret);
    }
}
#include "module.h"
module_init(dm_linear_init);

/* ── Stub: dm_linear_ctr ─────────────────────────────── */
int dm_linear_ctr(void *ti, unsigned int argc, char **argv)
{
    (void)ti;
    (void)argc;
    (void)argv;
    kprintf("[dm] dm_linear_ctr: not yet implemented\n");
    return 0;
}
/* ── Stub: dm_linear_map ─────────────────────────────── */
int dm_linear_map(void *ti, void *bio)
{
    (void)ti;
    (void)bio;
    kprintf("[dm] dm_linear_map: not yet implemented\n");
    return 0;
}
