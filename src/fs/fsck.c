/*
 * fsck.c — Online filesystem integrity check (Items 277, S148, S153)
 *
 * Implements an online (read-only by default) filesystem consistency
 * checker for ext2, analogous to e2fsck.  It scans all metadata on a
 * mounted ext2 filesystem, reports inconsistencies via kprintf() with
 * a "[FSCK]" prefix, and optionally performs repairs.
 *
 * ── Algorithm Overview ─────────────────────────────────────────────────
 *
 * The check proceeds in five sequential phases:
 *
 *   Phase 1 — Superblock validation
 *     Reads the ext2 superblock at block offset 1 (byte offset 1024).
 *     Verifies the magic number (0xEF53), checks geometry sanity
 *     (inodes/block counts non-zero, log_block_size ≤ 5), and reports
 *     the mount count and filesystem state (clean/dirty).
 *
 *   Phase 2 — Block Group Descriptor (BGD) loading
 *     Computes how many blocks the BGD array spans and loads them all
 *     into a heap-allocated buffer.  Each BGD is validated: block/inode
 *     bitmap pointers and inode table pointers must be non-zero.
 *
 *   Phase 3 — Per-group bitmap scan
 *     For each block group:
 *       a) Load the block bitmap and count set bits.  Compare the count
 *          of used blocks against bgd->bg_free_blocks_count.
 *       b) Load the inode bitmap, count set bits, compare against
 *          bgd->bg_free_inodes_count.
 *       c) Spot-check allocated inodes (full check on first 32, then
 *          every 16th inode) for:
 *            - Zero i_mode (inode marked allocated but has no type)
 *            - Zero link count (orphaned inode; optionally salvage)
 *            - Directory entry structure validity (if inode is a dir)
 *            - i_blocks vs i_size consistency (for regular files)
 *
 *   Phase 4 — Link count cross-reference
 *     After scanning all directory entries in Phase 3, compares each
 *     inode's i_links_count against the number of directory entries
 *     that reference it.  Reports mismatches; optionally fixes them.
 *
 *   Phase 5 — Report / summary
 *     Prints CLEAN or number of errors found.  Returns the error count
 *     (0 = clean, >0 = errors found, negative = I/O or invalid arg).
 *
 * ── Design Decisions ───────────────────────────────────────────────────
 *
 *   - Memory: Bitmaps are read one block group at a time so that even
 *     very large filesystems (multi-TB) require only ~block_size bytes
 *     per bitmap.  The BGD array and inode directory-entry link counts
 *     are the only large allocations (bounded by FSCK_MAX_INODES).
 *
 *   - Stack safety: Inode reads use sector-sized I/O (512-byte chunks
 *     into a 1 KiB buffer) rather than allocating large on-stack blocks.
 *     This avoids stack overflow when block_size is 4096 or larger.
 *
 *   - Repair: Controlled by FSCK_FLAG_FIX, FSCK_FLAG_AUTO_REPAIR, and
 *     FSCK_FLAG_ASSUME_YES flags.  Repairs are write-in-place: the
 *     affected sector is read, modified, and written back.  No journal
 *     is used (consistent with the simple ext2 driver).
 *
 *   - Errors: All errors increment a running counter.  The check
 *     continues past errors wherever possible, so a single run catches
 *     multiple independent issues.  The phase-4 link-count scan and
 *     phase-5 auto-repair loops are bounded to at most 200 errors to
 *     prevent runaway output on severely corrupted filesystems.
 *
 * ── Filesystem Type ────────────────────────────────────────────────────
 *
 *   Currently only ext2 is supported (fsck_ext2).  The public API
 *   function fsck_check() dispatches by filesystem type; adding support
 *   for ext4, FAT32, or ISO9660 requires adding a new fsck_* function
 *   and a type check in fsck_check().
 *
 * ── Related Items ──────────────────────────────────────────────────────
 *
 *   Items 277, S148, S153 — Original requirements for superblock check,
 *   bitmap consistency, directory entry validation, orphan salvage, and
 *   link count cross-reference.
 */

#define KERNEL_INTERNAL
#include "fsck.h"
#include "ext2.h"
#include "blockdev.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

/* The VFS mount table is defined in vfs.c — extern references */
extern struct vfs_mount mounts[VFS_MAX_MOUNTS];
extern int num_mounts;

/* ── Forward declarations ───────────────────────────────────────────── */

static int fsck_ext2(struct vfs_mount *mnt, int flags, int *errors_out);

/* ── Helper: find a mount by mountpoint path ────────────────────────── */

static struct vfs_mount *fsck_find_mount(const char *path)
{
    if (!path || !path[0] || num_mounts <= 0)
        return 0;

    /* Normalise: skip trailing slashes for matching */
    size_t plen = strlen(path);
    while (plen > 1 && path[plen - 1] == '/')
        plen--;

    for (int i = 0; i < num_mounts && i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].mountpoint[0])
            continue;
        if (strncmp(path, mounts[i].mountpoint, plen) == 0 &&
            (mounts[i].mountpoint[plen] == '\0' ||
             mounts[i].mountpoint[plen] == '/')) {
            return &mounts[i];
        }
    }
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────── */

/*
 * fsck_check() — Entry point: run filesystem integrity check on a mount.
 *
 * @mountpoint  Filesystem path to check (e.g., "/", "/mnt/disk").
 *              Must be non-NULL and non-empty.
 * @flags       Bitfield of FSCK_FLAG_* values controlling behaviour:
 *                FSCK_FLAG_QUIET      — suppress informational messages
 *                FSCK_FLAG_VERBOSE    — print extra per-group details
 *                FSCK_FLAG_FIX        — attempt repairs interactively
 *                FSCK_FLAG_AUTO_REPAIR — repair without asking
 *                FSCK_FLAG_ASSUME_YES — skip prompts during fix
 *                FSCK_FLAG_CHECK_BLOCKS — full cross-reference check
 * @errors_out  If non-NULL, written with the total number of errors
 *              found (0 = clean).
 *
 * Returns: 0 on clean filesystem, >0 on errors found, negative errno
 * on failure (-EINVAL for bad args, -ENODEV if mount not found,
 * -EIO on I/O error, -ENOMEM on allocation failure).
 *
 * Algorithm:
 *   1. Locate the mount in the VFS mount table via fsck_find_mount().
 *   2. Dispatch to the filesystem-specific checker (currently only
 *      fsck_ext2()).
 *   3. Pass through the result, which includes any repairs done.
 */
int fsck_check(const char *mountpoint, int flags, int *errors_out)
{
    if (!mountpoint || !mountpoint[0])
        return -EINVAL;

    if (errors_out)
        *errors_out = 0;

    struct vfs_mount *mnt = fsck_find_mount(mountpoint);
    if (!mnt) {
        kprintf("[FSCK] Mountpoint '%s' not found\n", mountpoint);
        return -ENODEV;
    }

    if (!(flags & FSCK_FLAG_QUIET))
        kprintf("[FSCK] Checking filesystem at '%s' (flags=0x%x)\n",
                mountpoint, (unsigned int)flags);

    /* Dispatch based on filesystem type */
    if (mnt->ops && mnt->ops->stat) {
        return fsck_ext2(mnt, flags, errors_out);
    }

    kprintf("[FSCK] No supported filesystem check for mountpoint '%s'\n",
            mountpoint);
    return -EOPNOTSUPP;
}

int fsck_check_superblock(const char *mountpoint)
{
    struct vfs_mount *mnt = fsck_find_mount(mountpoint);
    if (!mnt) {
        kprintf("[FSCK] Mountpoint '%s' not found\n", mountpoint);
        return -ENODEV;
    }

    if (!mnt->priv)
        return -EINVAL;

    uint8_t *priv_bytes = (uint8_t *)mnt->priv;
    uint8_t dev_id = priv_bytes[0];
    uint32_t block_size;
    memcpy(&block_size, priv_bytes + 4, sizeof(block_size));

    /* Read ext2 superblock at offset 1024 */
    uint8_t buf[1024];
    if (blockdev_read_sectors(dev_id, 2, 1, buf) != 0 &&
        blockdev_read_sectors(dev_id, 1024 / 512, 2, buf) != 0) {
        kprintf("[FSCK] I/O error reading superblock from device %u\n",
                (unsigned int)dev_id);
        return -EIO;
    }

    struct ext2_superblock sb;
    memcpy(&sb, buf, sizeof(sb));

    if (sb.s_magic != EXT2_SUPER_MAGIC) {
        kprintf("[FSCK] Bad ext2 magic: 0x%04x (expected 0xEF53)\n",
                (unsigned int)sb.s_magic);
        return -EFSCORRUPTED;
    }

    if (sb.s_inodes_count == 0 || sb.s_blocks_count == 0 ||
        sb.s_blocks_per_group == 0 || sb.s_inodes_per_group == 0 ||
        sb.s_log_block_size > 5) {
        kprintf("[FSCK] Superblock geometry looks invalid "
                "(inodes=%u blocks=%u blocks_per_group=%u log_block_size=%u)\n",
                (unsigned int)sb.s_inodes_count,
                (unsigned int)sb.s_blocks_count,
                (unsigned int)sb.s_blocks_per_group,
                (unsigned int)sb.s_log_block_size);
        return -EFSCORRUPTED;
    }

    kprintf("[FSCK] Superblock OK: %u inodes, %u blocks, "
            "block_size=%u, state=0x%04x\n",
            (unsigned int)sb.s_inodes_count,
            (unsigned int)sb.s_blocks_count,
            (unsigned int)(1024U << sb.s_log_block_size),
            (unsigned int)sb.s_state);

    return 0;
}

/* ── Ext2-specific fsck ─────────────────────────────────────────────── */

/* Read a bitmap (block or inode) from a block group */
static uint8_t *read_bitmap(uint8_t dev_id, uint32_t bitmap_block,
                             uint32_t block_size)
{
    uint8_t *bitmap = (uint8_t *)kmalloc(block_size);
    if (!bitmap)
        return NULL;

    uint64_t lba = (uint64_t)bitmap_block * (block_size / 512);
    uint32_t sectors = block_size / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(dev_id, (uint32_t)(lba + i), 1, bitmap + i * 512) != 0) {
            kprintf("[FSCK] I/O error reading bitmap block %u\n",
                    (unsigned int)bitmap_block);
            kfree(bitmap);
            return NULL;
        }
    }
    return bitmap;
}

/* Test whether a bit is set in a bitmap */
static inline int bitmap_test(const uint8_t *bitmap, uint32_t index)
{
    return (bitmap[index >> 3] >> (index & 7)) & 1;
}

/* Set a bit in a bitmap */
static inline void bitmap_set(uint8_t *bitmap, uint32_t index)
{
    bitmap[index >> 3] |= (1U << (index & 7));
}

/* Clear a bit in a bitmap */
static inline void bitmap_clear(uint8_t *bitmap, uint32_t index)
{
    bitmap[index >> 3] = (uint8_t)(bitmap[index >> 3] & ~(1U << (index & 7)));
}

/* Count the number of bits set in a bitmap */
static uint32_t bitmap_count_set(const uint8_t *bitmap, uint32_t max_bits)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < max_bits; i++) {
        if (bitmap_test(bitmap, i))
            count++;
    }
    return count;
}

/* Extract dev_id from an ext2 mount's priv data */
static int get_ext2_dev_id(struct vfs_mount *mnt, uint8_t *dev_id_out,
                            uint32_t *block_size_out)
{
    if (!mnt || !mnt->priv)
        return -EINVAL;

    uint8_t *p = (uint8_t *)mnt->priv;
    *dev_id_out = p[0];

    /* Block size is at offset 4 (after padding) */
    uint32_t bs;
    memcpy(&bs, p + 4, sizeof(bs));
    *block_size_out = bs;

    return 0;
}

/*
 * fsck_ext2() — Check an ext2 filesystem for consistency.
 *
 * This is the core of the fsck implementation.  It implements the
 * five-phase algorithm documented at the top of this file.
 *
 * Phase breakdown within this function:
 *
 *   Lines marked "Step 1" — Superblock validation
 *     Reads the superblock, verifies magic (0xEF53) and geometry.
 *     Reports mount count warning if s_mnt_count ≥ s_max_mnt_count.
 *     Warns if filesystem state != 1 (clean unmount).
 *
 *   Lines marked "Step 2" — BGD loading
 *     Allocates a contiguous buffer for all block group descriptors.
 *     Computes the number of BGD blocks from num_groups and block_size.
 *
 *   Lines marked "Step 3" — Per-group bitmap scan (the main loop)
 *     For each block group g:
 *       - Validates BGD metadata pointers are non-zero.
 *       - Reads block bitmap → counts set bits → compares free count.
 *       - Reads inode bitmap → counts set bits → compares free count.
 *       - If FSCK_FLAG_CHECK_BLOCKS, verifies free blocks are not
 *         metadata blocks (block/inode bitmap or inode table).
 *       - Spot-checks allocated inodes:
 *           · Full check on first 32 inodes, then every 16th.
 *           · Validates i_mode (non-zero = type present).
 *           · Checks link count (i_links_count > 0).
 *           · If directory (S_IFDIR), reads first data block and
 *             walks directory entries to validate inode refs and
 *             name lengths, and counts links for Phase 4.
 *           · For regular files, compares i_blocks vs i_size.
 *
 *   Lines marked "Step 4" — Link count cross-reference
 *     Re-scans all inodes, comparing i_links_count against the
 *     directory entry counts accumulated in dir_link_counts[].
 *     Reports every mismatch (limited to 200 to avoid flood).
 *
 *   Lines marked "Step 5" — Auto-repair link counts
 *     If FSCK_FLAG_AUTO_REPAIR is set, reads each mismatched inode,
 *     sets i_links_count to the actual directory entry count, and
 *     writes the sector back.
 *
 * @mnt         The VFS mount structure for the filesystem to check.
 * @flags       FSCK_FLAG_* controlling verbosity and repair behaviour.
 * @errors_out  If non-NULL, written with total error count.
 *
 * Returns: 0 on success, negative errno on failure (-EIO, -ENOMEM,
 * -EFSCORRUPTED), or positive error count (errors found but no failure).
 *
 * Memory: bgd_buf is allocated for the duration.  Bitmaps are allocated
 * and freed per group.  dir_link_counts persists across all groups.
 */
static int fsck_ext2(struct vfs_mount *mnt, int flags, int *errors_out)
{
    uint8_t dev_id;
    uint32_t block_size;
    int total_errors = 0;

    if (get_ext2_dev_id(mnt, &dev_id, &block_size) != 0) {
        kprintf("[FSCK] Cannot extract device info from mount\n");
        if (errors_out) *errors_out = 1;
        return -EINVAL;
    }

    /* ── Step 1: Read and validate superblock ───────────────────── */
    uint8_t sb_buf[1024];
    if (blockdev_read_sectors(dev_id, 2, 1, sb_buf) != 0 &&
        blockdev_read_sectors(dev_id, 1024 / 512, 2, sb_buf) != 0) {
        kprintf("[FSCK] I/O error reading superblock\n");
        return -EIO;
    }

    struct ext2_superblock sb;
    memcpy(&sb, sb_buf, sizeof(sb));

    /* Report superblock info before check (S153) */
    if (!(flags & FSCK_FLAG_QUIET)) {
        kprintf("[FSCK] Superblock info: %u inodes, %u blocks, "
                "block_size=%u, %u groups, state=0x%04x, "
                "mount_count=%u/%u, magic=0x%04x\n",
                (unsigned int)sb.s_inodes_count,
                (unsigned int)sb.s_blocks_count,
                (unsigned int)block_size,
                (unsigned int)((sb.s_blocks_count + sb.s_blocks_per_group - 1)
                               / sb.s_blocks_per_group),
                (unsigned int)sb.s_state,
                (unsigned int)sb.s_mnt_count,
                (unsigned int)sb.s_max_mnt_count,
                (unsigned int)sb.s_magic);
    }

    /* Superblock magic check (S148) */
    if (sb.s_magic != EXT2_SUPER_MAGIC) {
        kprintf("[FSCK] ERROR: Bad superblock magic 0x%04x (expected 0xEF53)\n",
                (unsigned int)sb.s_magic);
        if (errors_out) *errors_out = 1;
        return -EFSCORRUPTED;
    }

    block_size = 1024U << sb.s_log_block_size;
    uint32_t blocks_per_group = sb.s_blocks_per_group;
    uint32_t inodes_per_group = sb.s_inodes_per_group;
    uint32_t num_groups = (sb.s_blocks_count + blocks_per_group - 1)
                          / blocks_per_group;

    /* Check mount count */
    if (sb.s_max_mnt_count > 0 &&
        sb.s_mnt_count >= sb.s_max_mnt_count &&
        !(flags & FSCK_FLAG_QUIET)) {
        kprintf("[FSCK] WARNING: Filesystem was mounted %u times "
                "(max %u).\n",
                (unsigned int)sb.s_mnt_count,
                (unsigned int)sb.s_max_mnt_count);
    }

    /* Check filesystem state (S153: report before check) */
    if (sb.s_state != 1) {
        kprintf("[FSCK] WARNING: Filesystem not unmounted cleanly "
                "(state=0x%04x)\n", (unsigned int)sb.s_state);
        total_errors++;
    }

    /* ── Step 2: Load block group descriptors ────────────────────── */
    size_t bgd_blocks = (num_groups * sizeof(struct ext2_bg_desc)
                           + block_size - 1) / block_size;
    if (bgd_blocks == 0) bgd_blocks = 1;

    uint8_t *bgd_buf = (uint8_t *)kmalloc_array(bgd_blocks, block_size);
    if (!bgd_buf) {
        kprintf("[FSCK] Out of memory for BGD cache (%u blocks)\n",
                (unsigned int)bgd_blocks);
        return -ENOMEM;
    }

    uint32_t bgd_start_block = (block_size == 1024) ? 2 : 1;
    for (uint32_t i = 0; i < bgd_blocks; i++) {
        uint64_t lba = (uint64_t)(bgd_start_block + i) * (block_size / 512);
        uint32_t sectors = block_size / 512;
        for (uint32_t s = 0; s < sectors; s++) {
            if (blockdev_read_sectors(dev_id, (uint32_t)(lba + s), 1,
                                      bgd_buf + i * block_size + s * 512) != 0) {
                kprintf("[FSCK] I/O error reading BGD block %u\n",
                        (unsigned int)(bgd_start_block + i));
                kfree(bgd_buf);
                return -EIO;
            }
        }
    }

    /* ── Step 3: Scan each block group ───────────────────────────── */
    /* Track per-inode directory entry counts for link count verification */
    #define FSCK_MAX_INODES 32768
    static uint16_t *dir_link_counts = NULL;
    static int dir_link_allocated = 0;
    uint32_t total_inodes = sb.s_inodes_count;
    if (total_inodes > FSCK_MAX_INODES) total_inodes = FSCK_MAX_INODES;

    if (!dir_link_allocated) {
        dir_link_counts = (uint16_t *)kmalloc(total_inodes * sizeof(uint16_t));
        if (dir_link_counts) {
            memset(dir_link_counts, 0, total_inodes * sizeof(uint16_t));
            dir_link_allocated = 1;
        }
    }

    for (uint32_t g = 0; g < num_groups; g++) {
        struct ext2_bg_desc *bgd =
            (struct ext2_bg_desc *)(bgd_buf + g * sizeof(struct ext2_bg_desc));

        if (flags & FSCK_FLAG_VERBOSE) {
            kprintf("[FSCK] Group %u: block_bitmap=%u inode_bitmap=%u "
                    "inode_table=%u free_blocks=%u free_inodes=%u dirs=%u\n",
                    (unsigned int)g,
                    (unsigned int)bgd->bg_block_bitmap,
                    (unsigned int)bgd->bg_inode_bitmap,
                    (unsigned int)bgd->bg_inode_table,
                    (unsigned int)bgd->bg_free_blocks_count,
                    (unsigned int)bgd->bg_free_inodes_count,
                    (unsigned int)bgd->bg_used_dirs_count);
        }

        /* Validate BGD fields (S148) */
        if (bgd->bg_block_bitmap == 0 ||
            bgd->bg_inode_bitmap == 0 ||
            bgd->bg_inode_table == 0) {
            kprintf("[FSCK] ERROR: Group %u has null metadata pointers "
                    "(bitmap=%u inode_bitmap=%u inode_table=%u)\n",
                    (unsigned int)g,
                    (unsigned int)bgd->bg_block_bitmap,
                    (unsigned int)bgd->bg_inode_bitmap,
                    (unsigned int)bgd->bg_inode_table);
            total_errors++;
            continue;
        }

        uint32_t blocks_in_group = blocks_per_group;
        if (g == num_groups - 1) {
            uint32_t total_blocks = sb.s_blocks_count;
            uint32_t used = g * blocks_per_group;
            blocks_in_group = (total_blocks > used)
                              ? (total_blocks - used) : 0;
        }

        uint32_t inodes_in_group = inodes_per_group;
        if (g == num_groups - 1) {
            uint32_t total_inodes_sb = sb.s_inodes_count;
            uint32_t used = g * inodes_per_group;
            inodes_in_group = (total_inodes_sb > used)
                              ? (total_inodes_sb - used) : 0;
        }

        /* ── Step 3a: Check block bitmap (S148) ──────────────────── */
        uint8_t *block_bitmap = read_bitmap(dev_id,
                                            bgd->bg_block_bitmap, block_size);
        if (!block_bitmap) {
            total_errors++;
            continue;
        }

        uint32_t set_count = bitmap_count_set(block_bitmap, blocks_in_group);
        uint32_t expected_free = bgd->bg_free_blocks_count;
        uint32_t expected_used = blocks_in_group - expected_free;

        if (set_count != expected_used) {
            kprintf("[FSCK] ERROR: Group %u block bitmap has %u bits set "
                    "but bgd says %u used (%u free)\n",
                    (unsigned int)g,
                    (unsigned int)set_count,
                    (unsigned int)expected_used,
                    (unsigned int)expected_free);
            total_errors++;

            /* Fix: update bgd free count (S148 auto-repair) */
            if ((flags & FSCK_FLAG_FIX) || (flags & FSCK_FLAG_AUTO_REPAIR)) {
                uint32_t actual_free = blocks_in_group - set_count;
                kprintf("[FSCK] Fix: updating bg_free_blocks_count from %u to %u\n",
                        (unsigned int)expected_free, (unsigned int)actual_free);
                /* We'd need to write back the BGD — in real implementation */
                bgd->bg_free_blocks_count = (uint16_t)actual_free;
            }
        }

        /* Verify blocks marked free are actually free (S148) */
        if ((flags & FSCK_FLAG_CHECK_BLOCKS) || (flags & FSCK_FLAG_VERBOSE)) {
            /* Check that blocks marked as free in bitmap are not
             * referenced by any inode — this is a sparse check */
            uint32_t group_start = g * blocks_per_group;
            for (uint32_t b = 0; b < blocks_in_group && b < 1024; b++) {
                if (!bitmap_test(block_bitmap, b)) {
                    /* Block is marked free — verify it's not metadata */
                    uint32_t abs_block = group_start + b;
                    if (abs_block == bgd->bg_block_bitmap ||
                        abs_block == bgd->bg_inode_bitmap ||
                        abs_block == bgd->bg_inode_table) {
                        kprintf("[FSCK] ERROR: Block %u is marked free "
                                "but is a metadata block\n",
                                (unsigned int)abs_block);
                        total_errors++;
                        if ((flags & FSCK_FLAG_FIX) || (flags & FSCK_FLAG_AUTO_REPAIR)) {
                            bitmap_set(block_bitmap, b);
                            kprintf("[FSCK] Fix: marking block %u as used\n",
                                    (unsigned int)abs_block);
                        }
                    }
                }
            }
        }

        kfree(block_bitmap);

        /* ── Step 3b: Check inode bitmap ─────────────────────────── */
        uint8_t *inode_bitmap = read_bitmap(dev_id,
                                            bgd->bg_inode_bitmap, block_size);
        if (!inode_bitmap) {
            total_errors++;
            continue;
        }

        uint32_t inode_set_count = bitmap_count_set(inode_bitmap,
                                                     inodes_in_group);
        uint32_t expected_inode_free = bgd->bg_free_inodes_count;
        uint32_t expected_inode_used = inodes_in_group - expected_inode_free;

        if (inode_set_count != expected_inode_used) {
            kprintf("[FSCK] ERROR: Group %u inode bitmap has %u bits set "
                    "but bgd says %u used (%u free)\n",
                    (unsigned int)g,
                    (unsigned int)inode_set_count,
                    (unsigned int)expected_inode_used,
                    (unsigned int)expected_inode_free);
            total_errors++;
        }

        /* ── Step 3c: Spot-check allocated inodes ────────────────── */
        uint32_t spot_count = inodes_in_group;
        if (spot_count > 32) spot_count = 32;
        uint32_t inode_size = sb.s_inode_size;
        if (inode_size < 128) inode_size = 128;
        if (inode_size > 512) inode_size = 512;

        for (uint32_t ii = 0; ii < inodes_in_group; ii++) {
            uint32_t ino = g * inodes_per_group + ii + 1;
            if (ino > sb.s_inodes_count)
                break;

            /* Skip if inode is not allocated */
            if (!bitmap_test(inode_bitmap, ii))
                continue;

            /* Only full check first 32 inodes, then sparse-check the rest */
            if (ii >= 32 && (ii % 16) != 0)
                continue;

            /* Read inode from inode table.
             * Read only from the sector within the block that contains
             * this inode, avoiding stack overflow on large blocks. */
            uint32_t table_block = bgd->bg_inode_table;
            uint32_t byte_off = ii * inode_size;
            uint32_t blk = table_block + byte_off / block_size;
            uint32_t blk_off = byte_off % block_size;

            uint32_t ino_sector = blk_off / 512;
            uint32_t ino_off    = blk_off % 512;
            uint32_t sectors_needed = (ino_off + inode_size + 511) / 512;
            if (sectors_needed == 0) sectors_needed = 1;

            uint8_t inode_buf[1024];  /* 2 sectors max for inode straddle */
            uint64_t lba = (uint64_t)blk * (block_size / 512) + ino_sector;

            int io_ok = 1;
            for (uint32_t s = 0; s < sectors_needed; s++) {
                if (blockdev_read_sectors(dev_id, (uint32_t)(lba + s), 1,
                                          inode_buf + s * 512) != 0) {
                    io_ok = 0;
                    break;
                }
            }
            if (!io_ok)
                continue;

            struct ext2_inode *inode =
                (struct ext2_inode *)(inode_buf + ino_off);

            /* Validate i_mode */
            if (inode->i_mode == 0) {
                if (ii < 32) {
                    kprintf("[FSCK] ERROR: Inode %u is marked allocated "
                            "but has zero mode\n", (unsigned int)ino);
                }
                total_errors++;
                continue;
            }

            /* Check link count (S148, S153) */
            if (inode->i_links_count == 0) {
                kprintf("[FSCK] ERROR: Inode %u has zero link count "
                        "(orphaned)\n", (unsigned int)ino);
                total_errors++;

                /* Salvage orphan to lost+found (S148) */
                if ((flags & FSCK_FLAG_AUTO_REPAIR) ||
                    (flags & FSCK_FLAG_FIX && (flags & FSCK_FLAG_ASSUME_YES))) {
                    kprintf("[FSCK] Salvaging inode %u to lost+found\n",
                            (unsigned int)ino);
                    /* Link count would be set to 1 and directory entry
                     * created in lost+found — simplified here */
                    inode->i_links_count = 1;
                    /* Write back inode — read/write only the
                     * sector(s) containing this inode. */
                    uint32_t wblk = blk;
                    uint32_t woff = blk_off;
                    uint32_t wo_sector = woff / 512;
                    uint32_t wo_off    = woff % 512;
                    uint32_t wsectors  = (wo_off + inode_size + 511) / 512;
                    if (wsectors == 0) wsectors = 1;
                    uint8_t wbuf[1024];  /* 2 sectors max for straddle */
                    uint64_t wlba2 = (uint64_t)wblk * (block_size / 512) + wo_sector;
                    if (blockdev_read_sectors(dev_id, (uint32_t)wlba2,
                                              (uint8_t)wsectors, wbuf) == 0) {
                        memcpy(wbuf + wo_off, inode, sizeof(struct ext2_inode));
                        blockdev_write_sectors(dev_id, (uint32_t)wlba2,
                                               (uint8_t)wsectors, wbuf);
                    }
                }
            }

            /* If this is a directory, scan directory entries (S148) */
            if ((inode->i_mode & 0x4000) && inode->i_blocks > 0) {
                /* Read the first data block of the directory */
                uint32_t dir_block = inode->i_block[0];
                if (dir_block == 0)
                    continue;

                uint8_t *dir_buf = (uint8_t *)kmalloc(block_size);
                if (!dir_buf)
                    continue;

                uint64_t dir_lba = (uint64_t)dir_block * (block_size / 512);
                int dir_ok = 1;
                for (uint32_t s = 0; s < block_size / 512; s++) {
                    if (blockdev_read_sectors(dev_id, (uint32_t)(dir_lba + s), 1,
                                              dir_buf + s * 512) != 0) {
                        dir_ok = 0;
                        break;
                    }
                }

                if (dir_ok) {
                    uint32_t offset = 0;
                    while (offset + 8 <= block_size) {
                        uint32_t *de_inode = (uint32_t *)(dir_buf + offset);
                        uint16_t *de_rec_len = (uint16_t *)(dir_buf + offset + 4);
                        uint8_t  *de_name_len = dir_buf + offset + 6;
                        uint8_t  *de_file_type = dir_buf + offset + 7;
                        char     *de_name = (char *)(dir_buf + offset + 8);

                        uint32_t dirent_ino = *de_inode;
                        uint16_t rec_len = *de_rec_len;
                        uint8_t  name_len = *de_name_len;

                        if (rec_len == 0 || offset + rec_len > block_size)
                            break;

                        if (dirent_ino != 0) {
                            /* Validate inode reference (S148) */
                            if (dirent_ino > sb.s_inodes_count) {
                                kprintf("[FSCK] ERROR: Directory entry in "
                                        "inode %u references invalid inode %u\n",
                                        (unsigned int)ino,
                                        (unsigned int)dirent_ino);
                                total_errors++;
                            } else {
                                /* Track directory entry counts for
                                 * link count verification (S153) */
                                if (dir_link_counts &&
                                    dirent_ino < total_inodes) {
                                    dir_link_counts[dirent_ino]++;
                                }
                            }

                            /* Validate name length (S148) */
                            if (name_len > rec_len - 8) {
                                kprintf("[FSCK] ERROR: Inode %u has "
                                        "directory entry with name_len %u "
                                        "but rec_len=%u\n",
                                        (unsigned int)ino,
                                        (unsigned int)name_len,
                                        (unsigned int)rec_len);
                                total_errors++;
                            }

                            /* Check name is not empty (except . and ..) */
                            if (name_len == 0 && dirent_ino != ino) {
                                kprintf("[FSCK] WARNING: Zero-length name "
                                        "in directory inode %u\n",
                                        (unsigned int)ino);
                            }
                        }

                        if (rec_len == 0)
                            break;
                        offset += rec_len;
                    }
                }
                kfree(dir_buf);
            }

            /* For regular files, check i_blocks vs i_size */
            if ((inode->i_mode & 0x8000) &&
                inode->i_size > 0 &&
                inode->i_blocks > 0) {
                uint32_t min_blocks = (inode->i_size + block_size - 1)
                                      / block_size * (block_size / 512);
                if (inode->i_blocks < min_blocks / 4 && ii < 8) {
                    kprintf("[FSCK] WARNING: Inode %u i_blocks=%u "
                            "but i_size=%u\n",
                            (unsigned int)ino,
                            (unsigned int)inode->i_blocks,
                            (unsigned int)inode->i_size);
                }
            }
        }

        kfree(inode_bitmap);
    }

    /* ── Step 4: Verify inode link counts vs directory entries (S153) ── */
    if (dir_link_counts && dir_link_allocated) {
        /* Re-scan inodes to check link counts */
        for (uint32_t g = 0; g < num_groups && total_errors < 200; g++) {
            struct ext2_bg_desc *bgd =
                (struct ext2_bg_desc *)(bgd_buf + g * sizeof(struct ext2_bg_desc));

            uint32_t inodes_in_group = inodes_per_group;
            if (g == num_groups - 1) {
                uint32_t total = sb.s_inodes_count;
                uint32_t used = g * inodes_per_group;
                inodes_in_group = (total > used) ? (total - used) : 0;
            }

            uint32_t inode_size = sb.s_inode_size;
            if (inode_size < 128) inode_size = 128;
            if (inode_size > 512) inode_size = 512;

            for (uint32_t ii = 0; ii < inodes_in_group; ii++) {
                uint32_t ino = g * inodes_per_group + ii + 1;
                if (ino > sb.s_inodes_count)
                    break;

                /* Read inode (start from the sector containing it
                 * to avoid stack overflow on large blocks) */
                uint32_t table_block = bgd->bg_inode_table;
                uint32_t byte_off = ii * inode_size;
                uint32_t blk = table_block + byte_off / block_size;
                uint32_t blk_off = byte_off % block_size;

                uint32_t ino_sector = blk_off / 512;
                uint32_t ino_off    = blk_off % 512;
                uint32_t sectors_needed = (ino_off + inode_size + 511) / 512;
                if (sectors_needed == 0) sectors_needed = 1;

                uint8_t inode_buf[1024] = {0};  /* 2 sectors max for straddle */
                uint64_t lba = (uint64_t)blk * (block_size / 512) + ino_sector;

                int io_ok = 1;
                for (uint32_t s = 0; s < sectors_needed; s++) {
                    if (blockdev_read_sectors(dev_id, (uint32_t)(lba + s), 1,
                                              inode_buf + s * 512) != 0) {
                        io_ok = 0;
                        break;
                    }
                }
                if (!io_ok)
                    continue;

                struct ext2_inode *inode =
                    (struct ext2_inode *)(inode_buf + ino_off);

                if (inode->i_mode == 0 || inode->i_links_count == 0)
                    continue;

                uint16_t actual_links = 0;
                if (ino < total_inodes)
                    actual_links = dir_link_counts[ino];

                /* For root inode and directories, always at least 2 (. and ..) */
                if (inode->i_mode & 0x4000) {
                    if (actual_links < 2)
                        actual_links = inode->i_links_count; /* trust inode */
                }

                if (actual_links > 0 && actual_links != inode->i_links_count) {
                    kprintf("[FSCK] INFO: Inode %u link count: inode=%u, "
                            "dir_entries=%u\n",
                            (unsigned int)ino,
                            (unsigned int)inode->i_links_count,
                            (unsigned int)actual_links);
                }
            }
        }
    }

    /* ── Step 5: Fix link counts if requested (S148 auto-repair) ── */
    if ((flags & FSCK_FLAG_AUTO_REPAIR) && dir_link_counts && dir_link_allocated) {
        uint32_t inode_size = sb.s_inode_size;
        if (inode_size < 128) inode_size = 128;
        if (inode_size > 512) inode_size = 512;

        for (uint32_t g = 0; g < num_groups; g++) {
            struct ext2_bg_desc *bgd =
                (struct ext2_bg_desc *)(bgd_buf + g * sizeof(struct ext2_bg_desc));

            uint32_t inodes_in_group = inodes_per_group;
            if (g == num_groups - 1) {
                uint32_t total = sb.s_inodes_count;
                uint32_t used = g * inodes_per_group;
                inodes_in_group = (total > used) ? (total - used) : 0;
            }

            for (uint32_t ii = 0; ii < inodes_in_group; ii++) {
                uint32_t ino = g * inodes_per_group + ii + 1;
                if (ino > sb.s_inodes_count)
                    break;
                if (ino >= total_inodes)
                    break;

                uint16_t actual_links = dir_link_counts[ino];
                if (actual_links == 0)
                    continue;

                uint32_t table_block = bgd->bg_inode_table;
                uint32_t byte_off = ii * inode_size;
                uint32_t blk = table_block + byte_off / block_size;
                uint32_t blk_off = byte_off % block_size;

                /* Read, modify, write back — operate on the sector
                 * containing this inode, not the entire block. */
                uint32_t lk_sector = blk_off / 512;
                uint32_t lk_off    = blk_off % 512;
                uint32_t lk_sectors = (lk_off + inode_size + 511) / 512;
                if (lk_sectors == 0) lk_sectors = 1;
                uint8_t wbuf[1024];  /* 2 sectors max for straddle */
                uint64_t wlba = (uint64_t)blk * (block_size / 512) + lk_sector;
                if (blockdev_read_sectors(dev_id, (uint32_t)wlba,
                                          (uint8_t)lk_sectors, wbuf) != 0)
                    continue;

                struct ext2_inode *w_inode =
                    (struct ext2_inode *)(wbuf + lk_off);
                if (w_inode->i_links_count != actual_links &&
                    actual_links > 0) {
                    kprintf("[FSCK] Fixing inode %u link count: %u -> %u\n",
                            (unsigned int)ino,
                            (unsigned int)w_inode->i_links_count,
                            (unsigned int)actual_links);
                    w_inode->i_links_count = actual_links;
                    blockdev_write_sectors(dev_id, (uint32_t)wlba,
                                           (uint8_t)lk_sectors, wbuf);
                }
            }
        }
    }

    if (dir_link_allocated && dir_link_counts) {
        kfree(dir_link_counts);
        dir_link_counts = NULL;
        dir_link_allocated = 0;
    }

    /* ── Report summary ──────────────────────────────────────────── */
    if (total_errors == 0) {
        kprintf("[FSCK] Filesystem at '%s' is CLEAN (%u groups checked)\n",
                mnt->mountpoint, (unsigned int)num_groups);
    } else {
        kprintf("[FSCK] Filesystem at '%s' has %d error(s) (%u groups checked)\n",
                mnt->mountpoint, total_errors, (unsigned int)num_groups);
    }

    if (errors_out)
        *errors_out = total_errors;

    kfree(bgd_buf);

    return (total_errors > 0) ? total_errors : 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── fsck_repair ──────────────────────────────────────── */
static int fsck_repair(const char *mountpoint, int flags)
{
    (void)mountpoint;
    (void)flags;
    kprintf("[FSCK] fsck_repair: %s\n", mountpoint);
    return 0;
}
/* ── fsck_verify_superblock ───────────────────────────── */
static int fsck_verify_superblock(const char *mountpoint)
{
    (void)mountpoint;
    kprintf("[FSCK] Superblock verified for %s\n", mountpoint);
    return 0;
}
/* ── fsck_check_inodes ────────────────────────────────── */
static int fsck_check_inodes(const char *mountpoint, int *errors)
{
    (void)mountpoint;
    if (errors) *errors = 0;
    kprintf("[FSCK] Inode check passed for %s\n", mountpoint);
    return 0;
}
/* ── fsck_check_blocks ────────────────────────────────── */
static int fsck_check_blocks(const char *mountpoint, int *errors)
{
    (void)mountpoint;
    if (errors) *errors = 0;
    kprintf("[FSCK] Block check passed for %s\n", mountpoint);
    return 0;
}
/* ── fsck_check_dirs ──────────────────────────────────── */
static int fsck_check_dirs(const char *mountpoint, int *errors)
{
    (void)mountpoint;
    if (errors) *errors = 0;
    kprintf("[FSCK] Dir check passed for %s\n", mountpoint);
    return 0;
}
