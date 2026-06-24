#ifndef FSCK_H
#define FSCK_H

#include "types.h"

/*
 * Online filesystem integrity check (fsck).
 *
 * Scans the ext2 filesystem's inode and block bitmaps for inconsistencies,
 * validates inode fields, checks directory entry integrity, and reports
 * any issues found.  Optionally fixes simple problems (e.g., clearing
 * orphaned inode references).
 *
 * Return values:
 *   0  = filesystem is clean (no errors)
 *   >0 = number of errors detected (may list them via kprintf)
 *   <0 = error accessing the filesystem (I/O error, etc.)
 */

/* Flags for fsck_check() */
#define FSCK_FLAG_VERBOSE   (1U << 0)  /* log each check step */
#define FSCK_FLAG_QUIET     (1U << 1)  /* only report errors */
#define FSCK_FLAG_FIX       (1U << 2)  /* attempt to fix simple errors */
#define FSCK_FLAG_AUTO_REPAIR (1U << 3) /* -a: auto-repair (fix+salvage orphans) */
#define FSCK_FLAG_CHECK_BLOCKS (1U << 4) /* -c: check all blocks */
#define FSCK_FLAG_FORCE     (1U << 5)  /* -f: force check even if clean */
#define FSCK_FLAG_ASSUME_YES (1U << 6) /* -y: assume yes to all prompts */

/* ── Public API ─────────────────────────────────────────────────────── */

/*
 * Run integrity check on an ext2 filesystem mounted at the given
 * mountpoint (e.g. "/").  The filesystem must be quiescent or the
 * results may be inconsistent.
 *
 * @mountpoint:  filesystem mountpoint path (e.g. "/", "/mnt")
 * @flags:       bitmask of FSCK_FLAG_* values
 * @errors_out:  optional pointer to receive error count
 *
 * Returns 0 on clean filesystem, positive error count, or negative errno.
 */
int fsck_check(const char *mountpoint, int flags, int *errors_out);

/*
 * Run a quick superblock sanity check only.  Useful as a rapid pre-check.
 *
 * @mountpoint:  filesystem mountpoint path
 *
 * Returns 0 if superblock looks valid, -1 if invalid, -errno on I/O error.
 */
int fsck_check_superblock(const char *mountpoint);

#endif /* FSCK_H */
