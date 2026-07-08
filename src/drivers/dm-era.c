/* dm-era.c — Device mapper era target.
 *
 * Tracks which blocks have been written since a checkpoint.
 * Each write causes the block's era bit to be set in the current era
 * bitmap.  On "checkpoint", the current era is saved (archived) and a
 * new era begins.  Users can query which blocks were written in a given era.
 *
 * Integration with dm framework via dm_register_target().
 */

#define KERNEL_INTERNAL
#include "dm.h"
#include "dm-era.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "export.h"
#include "errno.h"
#include "spinlock.h"
#include "blockdev.h"

/* ── Forward declarations ──────────────────────────────────────────── */

static int  era_ctr(struct dm_target *ti, int argc, const char **argv);
static void era_dtr(struct dm_target *ti);
static int  era_map(struct dm_target *ti, struct blk_request *req,
                    struct blk_request *mapped[], int *mapped_count);

/* ── Target operations ─────────────────────────────────────────────── */

static const struct dm_target_ops era_ops = {
    .name  = "era",
    .flags = 0,
    .ctr   = era_ctr,
    .dtr   = era_dtr,
    .map   = era_map,
};

/* ── Constructor ───────────────────────────────────────────────────────
 *
 * Table line format:
 *   start_sectors length_sectors era block_size backing_device
 *
 * Example:
 *   0 1048576 era 8 /dev/sda
 *
 * Arguments:
 *   argv[0] = "block_size" (sectors, e.g., 8 = 4KB)
 *   argv[1] = backing device path (e.g., "/dev/sda")
 */

static int era_ctr(struct dm_target *ti, int argc, const char **argv)
{
    if (argc < 2) {
        kprintf("[DM-ERA] ctr: need at least 2 args (block_size backing_dev)\n");
        return -EINVAL;
    }

    /* Parse block size (in sectors) */
    uint64_t block_size = 0;
    const char *s = argv[0];
    while (*s) {
        if (*s < '0' || *s > '9')
            return -EINVAL;
        block_size = block_size * 10 + (uint64_t)(*s - '0');
        s++;
    }

    if (block_size == 0) {
        kprintf("[DM-ERA] ctr: block_size must be > 0\n");
        return -EINVAL;
    }

    /* Look up backing device */
    const char *backing_path = argv[1];
    int backing_dev = blockdev_find_by_name(backing_path);
    if (backing_dev < 0) {
        kprintf("[DM-ERA] ctr: backing device '%s' not found\n",
                backing_path);
        return -ENODEV;
    }

    /* Allocate private data */
    struct dm_era_private *priv = (struct dm_era_private *)
        kmalloc(sizeof(struct dm_era_private));
    if (!priv) return -ENOMEM;

    memset(priv, 0, sizeof(*priv));

    priv->block_size      = block_size;
    priv->current_era     = 1;
    priv->era_count       = 1;
    priv->backing_dev_id  = backing_dev;

    /* Calculate number of blocks from device size */
    uint64_t dev_sectors = blockdev_get_sectors(backing_dev);
    priv->nr_blocks = dev_sectors / block_size;
    if (priv->nr_blocks > DM_ERA_MAX_BLOCKS)
        priv->nr_blocks = DM_ERA_MAX_BLOCKS;

    priv->max_archive_eras = 8;
    priv->era_archive = NULL;  /* no archive in simple implementation */

    memset(&priv->current_map, 0, sizeof(priv->current_map));

    ti->private = priv;

    kprintf("[DM-ERA] ctr: block_size=%llu sectors, nr_blocks=%llu, "
            "backing_dev=%d, range=[%llu, %llu)\n",
            (unsigned long long)block_size,
            (unsigned long long)priv->nr_blocks,
            backing_dev,
            (unsigned long long)ti->start,
            (unsigned long long)(ti->start + ti->length));

    return 0;
}

/* ── Destructor ─────────────────────────────────────────────────────── */

static void era_dtr(struct dm_target *ti)
{
    if (!ti || !ti->private) return;

    struct dm_era_private *priv = (struct dm_era_private *)ti->private;

    kprintf("[DM-ERA] dtr: era_count=%u, total_writes=%llu, "
            "checkpoints=%llu\n",
            priv->era_count,
            (unsigned long long)priv->total_writes,
            (unsigned long long)priv->checkpoints);

    kfree(priv);
    ti->private = NULL;
}

/* ── Map function ──────────────────────────────────────────────────────
 *
 * On WRITE: mark the block's bit in the current era bitmap, then
 * remap to the backing device.
 * On READ: pass through to the backing device.
 */

static int era_map(struct dm_target *ti, struct blk_request *req,
                   struct blk_request *mapped[], int *mapped_count)
{
    struct dm_era_private *priv = (struct dm_era_private *)ti->private;
    if (!priv) return -EINVAL;

    int backing_dev = priv->backing_dev_id;
    if (backing_dev < 0) {
        return -ENODEV;
    }

    /* Check if this is a write request */
    int is_write = (req->flags & BLK_REQ_WRITE) ? 1 : 0;

    if (is_write) {
        /* Calculate which block(s) this write covers */
        uint64_t lba = req->lba;
        uint32_t count = req->count;
        uint64_t end_lba = lba + count;

        uint64_t start_block = lba / priv->block_size;
        uint64_t end_block = (end_lba + priv->block_size - 1) / priv->block_size;

        if (end_block > priv->nr_blocks)
            end_block = priv->nr_blocks;

        /* Mark each block's bit in the current era bitmap */
        for (uint64_t b = start_block; b < end_block; b++) {
            int bit_idx = (int)(b & 63);
            int word_idx = (int)(b / 64);
            priv->current_map.bits[word_idx] |= (1ULL << bit_idx);
        }

        priv->writes_in_current_era += count;
        priv->total_writes += count;
    }

    /* Remap to backing device: same lba, same count, same buf
     * but different dev_id */
    req->dev_id = (uint8_t)backing_dev;

    /* Return the original request as the mapped request */
    mapped[0] = req;
    *mapped_count = 1;

    return 0;
}

/* ── Message handler ────────────────────────────────────────────────── */

int dm_era_message(struct dm_target *ti, const char *command,
                   char *reply, int max_reply)
{
    struct dm_era_private *priv = (struct dm_era_private *)ti->private;
    if (!priv) return -EINVAL;
    if (!command || !reply) return -EINVAL;

    /* Parse command */
    if (strcmp(command, "checkpoint") == 0) {
        /* Save current era, start a new one */
        priv->era_count++;
        priv->current_era = priv->era_count;
        memset(&priv->current_map, 0, sizeof(priv->current_map));
        priv->writes_in_current_era = 0;
        priv->checkpoints++;

        int n = snprintf(reply, (size_t)(max_reply > 0 ? max_reply : 0),
                         "checkpoint saved: era %u\n", priv->current_era);
        return n;
    }

    if (strcmp(command, "status") == 0) {
        int n = snprintf(reply, (size_t)(max_reply > 0 ? max_reply : 0),
                         "current_era: %u\n"
                         "era_count: %u\n"
                         "block_size: %llu\n"
                         "nr_blocks: %llu\n"
                         "writes_in_current_era: %llu\n"
                         "total_writes: %llu\n"
                         "checkpoints: %llu\n",
                         priv->current_era,
                         priv->era_count,
                         (unsigned long long)priv->block_size,
                         (unsigned long long)priv->nr_blocks,
                         (unsigned long long)priv->writes_in_current_era,
                         (unsigned long long)priv->total_writes,
                         (unsigned long long)priv->checkpoints);
        return n;
    }

    /* Parse "era <n>" query */
    if (strncmp(command, "era ", 4) == 0) {
        const char *numstr = command + 4;
        uint32_t era_num = 0;
        while (*numstr >= '0' && *numstr <= '9') {
            era_num = era_num * 10 + (uint32_t)(*numstr - '0');
            numstr++;
        }

        if (era_num == 0 || era_num > priv->era_count) {
            return snprintf(reply, (size_t)(max_reply > 0 ? max_reply : 0),
                            "era %u not found (current: %u)\n",
                            era_num, priv->current_era);
        }

        /* For the current era, return the current bitmap summary */
        if (era_num == priv->current_era) {
            int count = 0;
            for (int i = 0; i < (DM_ERA_MAX_BLOCKS + 63) / 64; i++) {
                count += __builtin_popcountll(priv->current_map.bits[i]);
            }
            return snprintf(reply, (size_t)(max_reply > 0 ? max_reply : 0),
                           "era %u: %d blocks written\n", era_num, count);
        }

        /* Archived era not stored in this simple implementation */
        return snprintf(reply, (size_t)(max_reply > 0 ? max_reply : 0),
                        "era %u: archived data not available\n", era_num);
    }

    return snprintf(reply, (size_t)(max_reply > 0 ? max_reply : 0),
                    "unknown command: %s\n", command);
}

/* ── Query helpers ──────────────────────────────────────────────────── */

int dm_era_block_in_era(struct dm_target *ti, uint32_t era, uint64_t block)
{
    struct dm_era_private *priv = (struct dm_era_private *)ti->private;
    if (!priv) return 0;
    if (block >= priv->nr_blocks) return 0;

    if (era == priv->current_era) {
        int word_idx = (int)(block / 64);
        int bit_idx  = (int)(block & 63);
        return (priv->current_map.bits[word_idx] >> bit_idx) & 1ULL;
    }

    /* Archived eras not stored in this simple implementation */
    return 0;
}

uint32_t dm_era_get_current_era(struct dm_target *ti)
{
    struct dm_era_private *priv = (struct dm_era_private *)ti->private;
    return priv ? priv->current_era : 0;
}

/* ── Initialisation ─────────────────────────────────────────────────── */

void dm_era_init(void)
{
    int ret = dm_register_target(&era_ops);
    if (ret == 0) {
        kprintf("[DM-ERA] target type 'era' registered\n");
    } else {
        kprintf("[DM-ERA] failed to register target type: %d\n", ret);
    }
}
#include "module.h"
module_init(dm_era_init);

/* ── Stub: dm_era_ctr ─────────────────────────────── */
static int dm_era_ctr(void *ti, unsigned int argc, char **argv)
{
    (void)ti;
    (void)argc;
    (void)argv;
    kprintf("[DM] dm_era_ctr: not yet implemented\n");
    return 0;
}
/* ── Stub: dm_era_map ─────────────────────────────── */
static int dm_era_map(void *ti, void *bio)
{
    (void)ti;
    (void)bio;
    kprintf("[DM] dm_era_map: not yet implemented\n");
    return 0;
}
