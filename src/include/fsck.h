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
#define FSCK_FLAG_VERBOSE   (1 << 0)  /* log each check step */
#define FSCK_FLAG_QUIET     (1 << 1)  /* only report errors */
#define FSCK_FLAG_FIX       (1 << 2)  /* attempt to fix simple errors */

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
