#ifndef DM_ERA_H
#define DM_ERA_H

#include "types.h"
#include "dm.h"

/* ── dm-era constants ──────────────────────────────────────────────── */

#define DM_ERA_MAX_BLOCKS   (64 * 1024)  /* up to 64K blocks tracked */

/* DM-era ioctl-like commands (used via the message interface) */
#define DM_ERA_STATUS      1     /* get era statistics */
#define DM_ERA_CHECKPOINT  2     /* save current era, start new one */
#define DM_ERA_QUERY       3     /* query which blocks belong to era <n> */

/* Era bitmap: one bit per block */
struct dm_era_bitmap {
    uint64_t bits[(DM_ERA_MAX_BLOCKS + 63) / 64];
};

/* Private data for a dm-era target instance */
struct dm_era_private {
    uint64_t  block_size;        /* block size in sectors */
    uint64_t  nr_blocks;         /* total number of blocks tracked */
    uint32_t  current_era;       /* current era number (starts at 1) */
    uint32_t  era_count;         /* total number of eras since creation */

    int       backing_dev_id;    /* blockdev ID of the backing device */

    /* Era bitmaps: which blocks were written in each era.
     * We store the current era bitmap and a list of checkpointed era bitmaps
     * for query.  For simplicity we keep the last N eras. */
    struct dm_era_bitmap current_map;   /* bitmap for the current era */
    struct dm_era_bitmap *era_archive;  /* archived era bitmaps (NULL for simple impl) */
    uint32_t  max_archive_eras;         /* how many eras to keep */

    /* Statistics */
    uint64_t  writes_in_current_era;    /* write count since last checkpoint */
    uint64_t  total_writes;             /* total writes tracked */
    uint64_t  checkpoints;              /* number of checkpoints performed */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialise the dm-era target type (registers with dm framework).
 * Called during kernel init. */
void dm_era_init(void);

/* Message handler: process era commands.
 * @ti:       The dm target instance
 * @command:  Message command string ("checkpoint", "era <n>", "status")
 * @reply:    Output buffer for reply text
 * @max_reply: Size of reply buffer
 * Returns 0 on success, negative errno on failure. */
int dm_era_message(struct dm_target *ti, const char *command,
                   char *reply, int max_reply);

/* Query: test if a block was written in a given era.
 * @ti:   The dm target instance
 * @era:  Era number to query
 * @block: Block index
 * Returns 1 if the block was written in that era, 0 otherwise. */
int dm_era_block_in_era(struct dm_target *ti, uint32_t era, uint64_t block);

/* Get current era number. */
uint32_t dm_era_get_current_era(struct dm_target *ti);

#endif /* DM_ERA_H */
