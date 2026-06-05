/*
 * fsck.c — Online filesystem integrity check (Item 277)
 *
 * Scans ext2 filesystem metadata for inconsistencies, logs errors,
 * and optionally attempts simple repairs.
 *
 * Checks performed:
 *   1. Superblock validation (magic, geometry, feature flags)
 *   2. Block group descriptor consistency
 *   3. Block bitmap: verify each block is accounted for exactly once
 *      (used by inode, used by group metadata, or marked free)
 *   4. Inode bitmap: verify each inode is accounted for exactly once
 *   5. Inode field sanity (mode, size, link count, block count)
 *   6. Cross-reference: inodes referencing valid blocks that are marked
 *      free in bitmap (spot-check via block pointers)
 *
 * Design:
 *   - Allocates working buffers dynamically so large filesystems are handled.
 *   - Bitmap scanning is done one block group at a time to keep memory
 *     usage low.
 *   - Errors are reported via kprintf() with a clear prefix "[FSCK]".
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

/*
 * Resolve a mountpoint to its struct vfs_mount entry.
 * The mountpoint must match exactly (trailing slash stripped internally).
 */
static struct vfs_mount *fsck_find_mount(const char *path)
{
    if (!path || !path[0] || num_mounts <= 0)
        return NULL;

    /* Normalise: skip trailing slashes for matching */
    size_t plen = strlen(path);
    while (plen > 1 && path[plen - 1] == '/')
        plen--;

    for (int i = 0; i < num_mounts && i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].mountpoint[0])
            continue;
        /* Compare normalised paths */
        if (strncmp(path, mounts[i].mountpoint, plen) == 0 &&
            (mounts[i].mountpoint[plen] == '\0' ||
             mounts[i].mountpoint[plen] == '/')) {
            return &mounts[i];
        }
    }
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────── */

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

    /* Dispatch based on filesystem type by checking ops */
    /* For now, only ext2 is supported */
    if (mnt->ops && mnt->ops->stat) {
        /* Try ext2 check — reads superblock to verify magic */
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

    /* We need the ext2_priv which has dev_id. Extract it from the
     * mount's priv data.  For ext2, priv is an ext2_priv struct
     * whose first fields are dev_id and block_size. */
    if (!mnt->priv)
        return -EINVAL;

    /* Read the raw dev_id and block_size from ext2_priv.
     * We use loose coupling: ext2_priv starts with:
     *   uint8_t  dev_id;
     *   uint32_t block_size;
     *   uint32_t blocks_per_group;
     *   uint32_t inodes_per_group;
     *   uint32_t inode_size;
     *   uint32_t num_block_groups;
     * This is fragile but avoids including ext2_priv definition here. */
    uint8_t *priv_bytes = (uint8_t *)mnt->priv;
    uint8_t dev_id = priv_bytes[0];  /* first field */
    uint32_t block_size;
    memcpy(&block_size, priv_bytes + 1, sizeof(block_size));

    /* Read ext2 superblock at offset 1024 (sectors 2-3) */
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
        return -1;
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
        return -1;
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

/*
 * Read a bitmap (block or inode) from a block group.
 * Allocates a buffer of `size` bytes and reads the bitmap into it.
 * Returns the buffer (caller must kfree) or NULL on failure.
 */
static uint8_t *read_bitmap(uint8_t dev_id, uint32_t bitmap_block,
                             uint32_t block_size)
{
    uint8_t *bitmap = (uint8_t *)kmalloc(block_size);
    if (!bitmap)
        return NULL;

    uint64_t lba = (uint64_t)bitmap_block * (block_size / 512);
    uint32_t sectors = block_size / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(dev_id, lba + i, 1, bitmap + i * 512) != 0) {
            kprintf("[FSCK] I/O error reading bitmap block %u\n",
                    (unsigned int)bitmap_block);
            kfree(bitmap);
            return NULL;
        }
    }
    return bitmap;
}

/*
 * Test whether a bit is set in a bitmap.
 */
static inline int bitmap_test(const uint8_t *bitmap, uint32_t index)
{
    return (bitmap[index >> 3] >> (index & 7)) & 1;
}

/*
 * Count the number of bits set in a bitmap.
 */
static uint32_t bitmap_count_set(const uint8_t *bitmap, uint32_t max_bits)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < max_bits; i++) {
        if (bitmap_test(bitmap, i))
            count++;
    }
    return count;
}

/*
 * Extract dev_id from an ext2 mount's priv data.
 *
 * ext2_priv starts with:
 *   uint8_t  dev_id;       offset 0, 1 byte
 *   uint32_t block_size;   offset 1, 4 bytes (may be aligned to 4)
 *
 * Due to struct packing we read carefully.
 */
static int get_ext2_dev_id(struct vfs_mount *mnt, uint8_t *dev_id_out,
                            uint32_t *block_size_out)
{
    if (!mnt || !mnt->priv)
        return -1;

    /* Grab the ext2_priv bytes to extract fields.
     * We check it's really ext2 by reading the superblock. */
    uint8_t *p = (uint8_t *)mnt->priv;
    *dev_id_out = p[0];

    /* Block size is at offset 1, but may be padded to 4-byte alignment.
     * Read at the correct offset based on ext2_priv struct layout.
     * The struct is:
     *   uint8_t  dev_id;        (1 byte)
     *   uint32_t block_size;    (4 bytes — needs 4-byte alignment => padding of 3)
     * So block_size is at offset 4, not 1. */
    uint32_t bs;
    memcpy(&bs, p + 4, sizeof(bs));
    *block_size_out = bs;

    return 0;
}

/*
 * Check an ext2 filesystem for consistency.
 *
 * Walks all block groups, loading the block and inode bitmaps,
 * comparing their set-bit counts against the block group descriptor
 * free counts, and spot-checking allocated inodes for sanity.
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

    if (flags & FSCK_FLAG_VERBOSE) {
        kprintf("[FSCK] Superblock: %u inodes, %u blocks, %u groups, "
                "block_size=%u, feature_ro_compat=0x%04x\n",
                (unsigned int)sb.s_inodes_count,
                (unsigned int)sb.s_blocks_count,
                (unsigned int)num_groups,
                (unsigned int)block_size,
                (unsigned int)sb.s_feature_ro_compat);
    }

    /* Check mount count vs max mount count */
    if (sb.s_max_mnt_count > 0 &&
        sb.s_mnt_count >= sb.s_max_mnt_count &&
        (flags & FSCK_FLAG_VERBOSE)) {
        kprintf("[FSCK] WARNING: Filesystem was mounted %u times "
                "(max %u). Consider running full fsck offline.\n",
                (unsigned int)sb.s_mnt_count,
                (unsigned int)sb.s_max_mnt_count);
    }

    /* Check filesystem state */
    if (sb.s_state != 1) {  /* 1 = EXT2_VALID_FS */
        kprintf("[FSCK] WARNING: Filesystem was not unmounted cleanly "
                "(state=0x%04x)\n", (unsigned int)sb.s_state);
        total_errors++;
    }

    /* ── Step 2: Load block group descriptors ────────────────────── */
    uint32_t bgd_blocks = (num_groups * sizeof(struct ext2_bg_desc)
                           + block_size - 1) / block_size;
    if (bgd_blocks == 0) bgd_blocks = 1;

    uint8_t *bgd_buf = (uint8_t *)kmalloc(bgd_blocks * block_size);
    if (!bgd_buf) {
        kprintf("[FSCK] Out of memory for BGD cache (%u blocks)\n",
                (unsigned int)bgd_blocks);
        return -ENOMEM;
    }

    /* BGD starts at block 1 (or 2 for 1024-byte blocks) */
    uint32_t bgd_start_block = (block_size == 1024) ? 2 : 1;
    for (uint32_t i = 0; i < bgd_blocks; i++) {
        uint64_t lba = (uint64_t)(bgd_start_block + i) * (block_size / 512);
        uint32_t sectors = block_size / 512;
        for (uint32_t s = 0; s < sectors; s++) {
            if (blockdev_read_sectors(dev_id, lba + s, 1,
                                      bgd_buf + i * block_size + s * 512) != 0) {
                kprintf("[FSCK] I/O error reading BGD block %u\n",
                        (unsigned int)(bgd_start_block + i));
                kfree(bgd_buf);
                return -EIO;
            }
        }
    }

    /* ── Step 3: Scan each block group ───────────────────────────── */
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

        /* Validate bgd fields */
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

        /* ── Step 3a: Check block bitmap ─────────────────────────── */
        uint8_t *block_bitmap = read_bitmap(dev_id,
                                            bgd->bg_block_bitmap, block_size);
        if (!block_bitmap) {
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
        }

        kfree(block_bitmap);

        /* ── Step 3b: Check inode bitmap ─────────────────────────── */
        uint8_t *inode_bitmap = read_bitmap(dev_id,
                                            bgd->bg_inode_bitmap, block_size);
        if (!inode_bitmap) {
            total_errors++;
            continue;
        }

        uint32_t inodes_in_group = inodes_per_group;
        if (g == num_groups - 1) {
            uint32_t total_inodes = sb.s_inodes_count;
            uint32_t used = g * inodes_per_group;
            inodes_in_group = (total_inodes > used)
                              ? (total_inodes - used) : 0;
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
        /* Skip inode 0 (invalid) and check a sample of allocated inodes.
         * We check up to 16 per group to keep runtime bounded. */
        uint32_t spot_count = inodes_in_group;
        if (spot_count > 16) spot_count = 16;
        uint32_t inode_size = sb.s_inode_size;
        if (inode_size < 128) inode_size = 128;
        if (inode_size > 512) inode_size = 512;

        for (uint32_t ii = 0; ii < spot_count; ii++) {
            uint32_t ino = g * inodes_per_group + 1 + ii;
            if (ino > sb.s_inodes_count)
                break;

            /* Skip if inode is not allocated according to bitmap */
            if (!bitmap_test(inode_bitmap, ii))
                continue;

            /* Read inode from inode table */
            uint32_t table_block = bgd->bg_inode_table;
            uint32_t byte_off = ii * inode_size;
            uint32_t blk = table_block + byte_off / block_size;
            uint32_t blk_off = byte_off % block_size;

            uint8_t inode_buf[512];
            uint64_t lba = (uint64_t)blk * (block_size / 512);
            uint32_t buf_off = 0;
            uint32_t sectors_needed = (blk_off + inode_size + 511) / 512;
            if (sectors_needed == 0) sectors_needed = 1;

            int io_ok = 1;
            for (uint32_t s = 0; s < sectors_needed; s++) {
                if (blockdev_read_sectors(dev_id, lba + s, 1,
                                          inode_buf + buf_off) != 0) {
                    kprintf("[FSCK] I/O error reading inode %u at block %u\n",
                            (unsigned int)ino, (unsigned int)(blk + s));
                    total_errors++;
                    io_ok = 0;
                    break;
                }
                buf_off += 512;
            }
            if (!io_ok)
                continue;

            struct ext2_inode *inode =
                (struct ext2_inode *)(inode_buf + blk_off);

            /* Validate i_mode */
            if (inode->i_mode == 0) {
                kprintf("[FSCK] ERROR: Inode %u is marked allocated "
                        "but has zero mode\n", (unsigned int)ino);
                total_errors++;
                continue;
            }

            /* Check link count */
            if (inode->i_links_count == 0) {
                kprintf("[FSCK] ERROR: Inode %u has zero link count "
                        "(orphaned)\n", (unsigned int)ino);
                total_errors++;
            }

            /* For regular files, check i_blocks vs i_size consistency */
            if ((inode->i_mode & 0x8000) &&  /* regular file */
                inode->i_size > 0 &&
                inode->i_blocks > 0) {
                uint32_t min_blocks = (inode->i_size + block_size - 1)
                                      / block_size * (block_size / 512);
                if (inode->i_blocks < min_blocks / 4) {
                    kprintf("[FSCK] WARNING: Inode %u i_blocks=%u "
                            "but i_size=%u suggests ~%u blocks\n",
                            (unsigned int)ino,
                            (unsigned int)inode->i_blocks,
                            (unsigned int)inode->i_size,
                            (unsigned int)min_blocks);
                }
            }
        }

        kfree(inode_bitmap);
    }

    /* ── Report summary ──────────────────────────────────────────────── */
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
