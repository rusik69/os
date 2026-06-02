#ifndef QUOTA_H
#define QUOTA_H

#include "types.h"
#include "errno.h"
#include "vfs.h"
#include "fs.h"

/*
 * ── Filesystem Quota System ─────────────────────────────────────────────
 *
 * Per-UID block and inode limits with soft/hard distinction and grace
 * periods.  Functions are called from the VFS layer (vfs_write, vfs_create,
 * vfs_unlink, vfs_truncate) to enforce limits.
 *
 * The struct fs_quota and base quota functions (vfs_set_quota,
 * vfs_get_quota, vfs_check_quota_blocks, vfs_check_quota_inodes)
 * are declared in <vfs.h> and implemented in quota.c.
 *
 * This header provides additional helpers and update functions.
 */

/* Block size for quota accounting (512 bytes, like traditional UNIX) */
#define QUOTA_BLOCK_SIZE  512

/* Convert byte size to QUOTA_BLOCK_SIZE blocks (rounding up) */
static inline uint32_t bytes_to_blocks(uint32_t bytes)
{
    return (bytes + QUOTA_BLOCK_SIZE - 1) / QUOTA_BLOCK_SIZE;
}

/* ── Extended API (not in vfs.h) ──────────────────────────────────────── */

/* Initialise the quota subsystem (called from vfs_enhance_init). */
void vfs_quota_init(void);

/*
 * Update block usage tracking after a successful write or truncate.
 * @delta_blocks: positive to add, negative to subtract.
 */
void vfs_update_quota_blocks(uint16_t uid, int32_t delta_blocks);

/*
 * Update inode usage tracking after a successful create or unlink.
 * @delta: +1 for create, -1 for unlink.
 */
void vfs_update_quota_inodes(uint16_t uid, int32_t delta);

/* Debug: print all active quota entries via kprintf. */
void vfs_quota_dump(void);

#endif /* QUOTA_H */
