/*
 * dm-raid.c — Device Mapper RAID target (RAID0/1/10)
 *
 * Implements RAID levels 0 (striping), 1 (mirroring), and 10
 * (striped mirrors) via the device mapper framework.
 *
 * Table format:
 *   start length raid0 <dev1_id> <dev2_id> [<devN_id>...] <stripe_size>
 *   start length raid1 <mirror_dev_id> <num_mirrors>
 *   start length raid10 <dev1_id> <dev2_id> ... <stripe_size>
 *
 * Examples:
 *   0 2097152 raid0 0 1 128       — RAID0 over dev 0 and 1, 128-sector stripe
 *   0 1048576 raid1 2 2            — RAID1 over 2 mirrors starting at dev 2
 *   0 4194304 raid10 0 1 2 3 256   — RAID10 over devs 0-3, 256-sector stripe
 *
 * Item 450: DM-RAID target (RAID0/1/10)
 */

#define KERNEL_INTERNAL
#include "dm.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "export.h"
#include "errno.h"
#include "blockdev.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define DM_RAID_MAX_DEVS     8   /* maximum disks in a RAID set */
#define DM_RAID_MIN_STRIPE   2   /* minimum stripe size in sectors */
#define DM_RAID_MAX_STRIPE   4096

/* RAID level identifiers */
#define DM_RAID_LEVEL_0   0
#define DM_RAID_LEVEL_1   1
#define DM_RAID_LEVEL_10  10

/* ── Private per-target data ───────────────────────────────────────── */

struct raid_private {
    int     level;                   /* RAID level */
    int     num_devs;                /* number of backing devices */
    int     dev_ids[DM_RAID_MAX_DEVS]; /* backing device IDs */
    uint64_t stripe_size;            /* stripe size in sectors (RAID0/10) */
    uint64_t stripe_sectors;         /* total sectors per stripe */
};

/* ── Forward declarations ─────────────────────────────────────────── */

static int  raid_ctr(struct dm_target *ti, int argc, const char **argv);
static void raid_dtr(struct dm_target *ti);
static int  raid_map(struct dm_target *ti, struct blk_request *req,
                     struct blk_request *mapped[], int *mapped_count);

/* ── Constructor ──────────────────────────────────────────────────── */

static int raid_ctr(struct dm_target *ti, int argc, const char **argv)
{
    if (argc < 2) {
        kprintf("[dm-raid] ctr: need at least 2 args (level devs...), got %d\n", argc);
        return -EINVAL;
    }

    struct raid_private *priv = (struct raid_private *)
        kmalloc(sizeof(struct raid_private));
    if (!priv) return -ENOMEM;
    memset(priv, 0, sizeof(*priv));

    /* Parse RAID level */
    const char *level_str = argv[0];
    if (strcmp(level_str, "raid0") == 0)
        priv->level = DM_RAID_LEVEL_0;
    else if (strcmp(level_str, "raid1") == 0)
        priv->level = DM_RAID_LEVEL_1;
    else if (strcmp(level_str, "raid10") == 0)
        priv->level = DM_RAID_LEVEL_10;
    else {
        kprintf("[dm-raid] ctr: unknown level '%s' (expected raid0/raid1/raid10)\n",
                level_str);
        kfree(priv);
        return -EINVAL;
    }

    /* Parse device IDs from remaining args */
    int dev_argc = argc - 1;  /* skip level */
    const char **dev_argv = argv + 1;

    priv->num_devs = 0;

    /* For RAID0/10, the last arg may be stripe size; for RAID1, last is num_mirrors */
    int stripe_arg_idx = dev_argc;

    for (int i = 0; i < dev_argc; i++) {
        /* Check if this looks like a number (all digits) */
        const char *s = dev_argv[i];
        int is_number = 1;
        for (const char *p = s; *p; p++) {
            if (*p < '0' || *p > '9') { is_number = 0; break; }
        }
        if (!is_number) {
            kprintf("[dm-raid] ctr: invalid argument '%s'\n", dev_argv[i]);
            kfree(priv);
            return -EINVAL;
        }

        /* Parse numeric */
        uint64_t val = 0;
        for (const char *p = s; *p; p++)
            val = val * 10 + (uint64_t)(*p - '0');

        if (priv->level == DM_RAID_LEVEL_1) {
            /* RAID1: first arg is starting dev_id, second is num_mirrors */
            if (i == 0) {
                if (!blockdev_is_registered((int)val)) {
                    kprintf("[dm-raid] ctr: device %llu not registered\n",
                            (unsigned long long)val);
                    kfree(priv);
                    return -ENODEV;
                }
                priv->dev_ids[0] = (int)val;
                priv->num_devs = 1;
            } else if (i == 1) {
                priv->num_devs = (int)val;
                if (priv->num_devs < 2 || priv->num_devs > DM_RAID_MAX_DEVS) {
                    kprintf("[dm-raid] ctr: RAID1 needs 2-%d mirrors, got %d\n",
                            DM_RAID_MAX_DEVS, priv->num_devs);
                    kfree(priv);
                    return -EINVAL;
                }
            }
            stripe_arg_idx = 1; /* no stripe size for RAID1 */
        } else {
            /* RAID0/10: collect devices, last numeric is stripe size */
            if (i < dev_argc - 1) {
                if (!blockdev_is_registered((int)val)) {
                    kprintf("[dm-raid] ctr: device %llu not registered\n",
                            (unsigned long long)val);
                    kfree(priv);
                    return -ENODEV;
                }
                if (priv->num_devs < DM_RAID_MAX_DEVS) {
                    priv->dev_ids[priv->num_devs++] = (int)val;
                } else {
                    kprintf("[dm-raid] ctr: too many devices (max %d)\n",
                            DM_RAID_MAX_DEVS);
                    kfree(priv);
                    return -EINVAL;
                }
            } else {
                /* This is the stripe size */
                priv->stripe_size = val;
                stripe_arg_idx = i;
            }
        }
    }

    /* Validate */
    if (priv->num_devs < 2) {
        kprintf("[dm-raid] ctr: need at least 2 devices\n");
        kfree(priv);
        return -EINVAL;
    }

    if (priv->level == DM_RAID_LEVEL_0 || priv->level == DM_RAID_LEVEL_10) {
        if (priv->stripe_size < DM_RAID_MIN_STRIPE ||
            priv->stripe_size > DM_RAID_MAX_STRIPE) {
            kprintf("[dm-raid] ctr: stripe_size %llu out of range [%d, %d]\n",
                    (unsigned long long)priv->stripe_size,
                    DM_RAID_MIN_STRIPE, DM_RAID_MAX_STRIPE);
            kfree(priv);
            return -EINVAL;
        }
    }

    priv->stripe_sectors = priv->stripe_size * priv->num_devs;

    ti->private = priv;

    kprintf("[dm-raid] ctr: level=%s, devs=%d, stripe=%llu\n",
            (priv->level == DM_RAID_LEVEL_0) ? "RAID0" :
            (priv->level == DM_RAID_LEVEL_1) ? "RAID1" : "RAID10",
            priv->num_devs, (unsigned long long)priv->stripe_size);
    return 0;
}

/* ── Destructor ───────────────────────────────────────────────────── */

static void raid_dtr(struct dm_target *ti)
{
    if (ti->private) {
        struct raid_private *priv = (struct raid_private *)ti->private;
        memset(priv, 0, sizeof(*priv));
        kfree(priv);
        ti->private = NULL;
    }
}

/* ── I/O Mapping ──────────────────────────────────────────────────── */

/* Map a request for RAID0 (striping).
 * Stripe size determines which device + offset the request goes to. */
static int raid0_map(struct raid_private *priv, struct blk_request *req,
                     struct blk_request *mapped[], int *mapped_count)
{
    uint64_t offset = req->lba;
    uint64_t stripe = offset / priv->stripe_sectors;
    uint64_t remainder = offset % priv->stripe_sectors;
    int dev_idx = (int)(stripe % priv->num_devs);
    uint64_t dev_stripe = stripe / priv->num_devs;
    uint64_t dev_lba = dev_stripe * priv->stripe_size + remainder;

    /* Clamp to device size */
    uint64_t dev_sectors = blockdev_get_sectors(priv->dev_ids[dev_idx]);
    if (dev_lba + req->count > dev_sectors) {
        req->result = -EIO;
        blk_request_done(req);
        *mapped_count = 0;
        return 0;
    }

    /* Create mapped request */
    struct blk_request *clone = blk_request_alloc();
    if (!clone) return -ENOMEM;
    memcpy(clone, req, sizeof(struct blk_request));
    clone->dev_id = (uint8_t)priv->dev_ids[dev_idx];
    clone->lba = dev_lba;
    mapped[0] = clone;
    *mapped_count = 1;
    return 0;
}

/* Map a request for RAID1 (mirroring).
 * Reads go to the first available device; writes go to all mirrors. */
static int raid1_map(struct raid_private *priv, struct blk_request *req,
                     struct blk_request *mapped[], int *mapped_count)
{
    uint64_t offset = req->lba;
    int num_mirrors = priv->num_devs;

    if (req->flags & BLK_REQ_WRITE) {
        /* Write to all mirrors */
        if (num_mirrors > DM_RAID_MAX_DEVS) num_mirrors = DM_RAID_MAX_DEVS;
        for (int i = 0; i < num_mirrors; i++) {
            struct blk_request *clone = blk_request_alloc();
            if (!clone) return -ENOMEM;
            memcpy(clone, req, sizeof(struct blk_request));
            clone->dev_id = (uint8_t)priv->dev_ids[i];
            clone->lba = offset;
            mapped[i] = clone;
        }
        *mapped_count = num_mirrors;
    } else {
        /* Read from first available device */
        for (int i = 0; i < num_mirrors; i++) {
            if (blockdev_is_registered(priv->dev_ids[i])) {
                struct blk_request *clone = blk_request_alloc();
                if (!clone) return -ENOMEM;
                memcpy(clone, req, sizeof(struct blk_request));
                clone->dev_id = (uint8_t)priv->dev_ids[i];
                clone->lba = offset;
                mapped[0] = clone;
                *mapped_count = 1;
                return 0;
            }
        }
        req->result = -EIO;
        blk_request_done(req);
        *mapped_count = 0;
    }
    return 0;
}

/* Map a request for RAID10 (striped mirrors).
 * Combines RAID0 striping with per-stripe mirroring. */
static int raid10_map(struct raid_private *priv, struct blk_request *req,
                      struct blk_request *mapped[], int *mapped_count)
{
    /* RAID10: half the devices are mirrors of the other half */
    int mirror_sets = priv->num_devs / 2;
    if (mirror_sets < 1) {
        req->result = -EINVAL;
        blk_request_done(req);
        *mapped_count = 0;
        return 0;
    }

    uint64_t offset = req->lba;
    uint64_t stripe = offset / priv->stripe_sectors;
    uint64_t remainder = offset % priv->stripe_sectors;
    int set_idx = (int)(stripe % mirror_sets);
    uint64_t dev_stripe = stripe / mirror_sets;
    uint64_t dev_lba = dev_stripe * priv->stripe_size + remainder;

    if (req->flags & BLK_REQ_WRITE) {
        /* Write to both mirrors in the set */
        int dev0 = priv->dev_ids[set_idx];
        int dev1 = priv->dev_ids[set_idx + mirror_sets];

        struct blk_request *clone0 = blk_request_alloc();
        if (!clone0) return -ENOMEM;
        memcpy(clone0, req, sizeof(struct blk_request));
        clone0->dev_id = (uint8_t)dev0;
        clone0->lba = dev_lba;
        mapped[0] = clone0;

        struct blk_request *clone1 = blk_request_alloc();
        if (!clone1) { kfree(clone0); return -ENOMEM; }
        memcpy(clone1, req, sizeof(struct blk_request));
        clone1->dev_id = (uint8_t)dev1;
        clone1->lba = dev_lba;
        mapped[1] = clone1;

        *mapped_count = 2;
    } else {
        /* Read from first available mirror in the set */
        int dev = priv->dev_ids[set_idx];
        if (!blockdev_is_registered(dev))
            dev = priv->dev_ids[set_idx + mirror_sets];
        if (!blockdev_is_registered(dev)) {
            req->result = -EIO;
            blk_request_done(req);
            *mapped_count = 0;
            return 0;
        }

        struct blk_request *clone = blk_request_alloc();
        if (!clone) return -ENOMEM;
        memcpy(clone, req, sizeof(struct blk_request));
        clone->dev_id = (uint8_t)dev;
        clone->lba = dev_lba;
        mapped[0] = clone;
        *mapped_count = 1;
    }
    return 0;
}

/* ── Target map ───────────────────────────────────────────────────── */

static int raid_map(struct dm_target *ti, struct blk_request *req,
                    struct blk_request *mapped[], int *mapped_count)
{
    struct raid_private *priv = (struct raid_private *)ti->private;
    if (!priv) return -EINVAL;

    switch (priv->level) {
    case DM_RAID_LEVEL_0:
        return raid0_map(priv, req, mapped, mapped_count);
    case DM_RAID_LEVEL_1:
        return raid1_map(priv, req, mapped, mapped_count);
    case DM_RAID_LEVEL_10:
        return raid10_map(priv, req, mapped, mapped_count);
    default:
        return -EINVAL;
    }
}

/* ── Target operations struct ─────────────────────────────────────── */

static const struct dm_target_ops raid_ops = {
    .name  = "raid",
    .flags = 0,
    .ctr   = raid_ctr,
    .dtr   = raid_dtr,
    .map   = raid_map,
};

/* ── Init / registration ──────────────────────────────────────────── */

void dm_raid_init(void)
{
    int ret = dm_register_target(&raid_ops);
    if (ret == 0) {
        kprintf("[OK] dm-raid: RAID0/1/10 target registered\n");
    } else {
        kprintf("[FAIL] dm-raid: registration failed: %d\n", ret);
    }
}
#include "module.h"
module_init(dm_raid_init);
