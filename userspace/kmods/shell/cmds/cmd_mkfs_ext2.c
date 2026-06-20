/*
 * cmd_mkfs_ext2.c — Create an ext2 filesystem
 *
 * Formats a block device with an ext2 filesystem, writing:
 *   - Superblock (with magic 0xEF53)
 *   - Block group descriptors
 *   - Block bitmap
 *   - Inode bitmap
 *   - Inode table (with root directory inode 2)
 *   - Root directory entries (. and ..)
 *
 * Usage: mkfs.ext2 [-b block-size] [-i bytes-per-inode] <device> [block-count]
 *
 * Item 146 — mkfs.ext2
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "errno.h"
#include "ext2.h"
#include "blockdev.h"

/* Note: libc.h intentionally omitted — ext2.h pulls in vfs.h which conflicts */

/* ── Block size constants ───────────────────────────────────────────── */

#define EXT2_MIN_BLOCK_SIZE  1024
#define EXT2_MAX_BLOCK_SIZE  4096
#define EXT2_DEFAULT_BLOCK_SIZE  4096
#define EXT2_DEFAULT_BYTES_PER_INODE 16384

/* ── Inode mode bits ────────────────────────────────────────────────── */

#define EXT2_S_IFREG  0100000  /* Regular file */
#define EXT2_S_IFDIR  0040000  /* Directory */
#define EXT2_S_IRWXU  00700    /* Owner rwx */
#define EXT2_S_IRUSR  00400
#define EXT2_S_IWUSR  00200
#define EXT2_S_IXUSR  00100
#define EXT2_S_IRWXG  00070    /* Group rwx */
#define EXT2_S_IRWXO  00007    /* Other rwx */

/* ── Constants for mkfs ─────────────────────────────────────────────── */

#define ROOT_INODE     2
#define EXT2_GOOD_OLD_REVISION 0
#define EXT2_DYNAMIC_REV       1
#define EXT2_INODE_SIZE_DEFAULT 128

/* ── Helper: div_round_up ──────────────────────────────────────────── */

static inline uint32_t div_round_up(uint32_t x, uint32_t y)
{
    return (x + y - 1) / y;
}

/* ── Main command ──────────────────────────────────────────────────── */

int cmd_mkfs_ext2(int argc, char **argv)
{
    const char *device = NULL;
    uint32_t block_size = EXT2_DEFAULT_BLOCK_SIZE;
    uint32_t bytes_per_inode = EXT2_DEFAULT_BYTES_PER_INODE;
    uint32_t block_count = 0;
    int dev_id = -1;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            block_size = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            bytes_per_inode = (uint32_t)atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            device = argv[i];
            if (i + 1 < argc && argv[i+1][0] != '-') {
                block_count = (uint32_t)atoi(argv[++i]);
            }
        }
    }

    if (!device) {
        kprintf("usage: mkfs.ext2 [-b block-size] [-i bytes-per-inode] <device> [block-count]\n");
        return 1;
    }

    /* Validate block size */
    if (block_size < EXT2_MIN_BLOCK_SIZE || block_size > EXT2_MAX_BLOCK_SIZE ||
        (block_size & (block_size - 1)) != 0) {
        kprintf("mkfs.ext2: invalid block size %u (must be power of 2, 1024-4096)\n",
                block_size);
        return 1;
    }

    /* Open device */
    dev_id = blockdev_find_by_name(device);
    if (dev_id < 0) {
        /* Try as a number (legacy blockdev ID) */
        dev_id = atoi(device);
    }

    if (dev_id < 0 || !blockdev_is_registered(dev_id)) {
        kprintf("mkfs.ext2: cannot open '%s'\n", device);
        return 1;
    }

    uint64_t total_sectors = blockdev_get_sectors(dev_id);
    if (block_count == 0) {
        block_count = (uint32_t)(total_sectors * 512 / block_size);
    }

    /* ── Filesystem parameters ───────────────────────────────────── */
    uint32_t blocks_per_group = block_size * 8;  /* 8 blocks per byte in bitmap */
    uint32_t inodes_per_group = div_round_up(blocks_per_group, bytes_per_inode / block_size);

    /* Ensure minimum inodes per group */
    if (inodes_per_group < 16) inodes_per_group = 16;

    uint32_t num_groups = div_round_up(block_count, blocks_per_group);
    uint32_t inode_size = EXT2_INODE_SIZE_DEFAULT;

    kprintf("mkfs.ext2: Creating ext2 on '%s'\n", device);
    kprintf("  Block size: %u\n", block_size);
    kprintf("  Block count: %u\n", block_count);
    kprintf("  Block groups: %u\n", num_groups);
    kprintf("  Blocks per group: %u\n", blocks_per_group);
    kprintf("  Inodes per group: %u\n", inodes_per_group);

    /* ── Allocate buffer ──────────────────────────────────────────── */
    uint8_t *buf = (uint8_t *)malloc(block_size);
    if (!buf) {
        kprintf("mkfs.ext2: out of memory\n");
        return 1;
    }
    memset(buf, 0, block_size);

    /* ── Write superblock ─────────────────────────────────────────── */
    /* Superblock sits at byte offset 1024 (block 0 or 1 depending on block size) */
    uint32_t sb_block = (block_size == 1024) ? 1 : 0;
    uint64_t sb_offset = (block_size == 1024) ? block_size : 0;  /* offset within block */

    struct ext2_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_inodes_count = num_groups * inodes_per_group;
    sb.s_blocks_count = block_count;
    sb.s_r_blocks_count = block_count / 20;  /* 5% reserved */
    sb.s_free_blocks_count = block_count - sb_block - 1;
    sb.s_free_inodes_count = sb.s_inodes_count - 1;  /* root inode used */
    sb.s_first_data_block = (block_size == 1024) ? 1 : 0;
    sb.s_log_block_size = 0;
    while ((1U << (sb.s_log_block_size + 10)) < block_size)
        sb.s_log_block_size++;
    sb.s_log_frag_size = sb.s_log_block_size;
    sb.s_blocks_per_group = blocks_per_group;
    sb.s_frags_per_group = blocks_per_group;
    sb.s_inodes_per_group = inodes_per_group;
    sb.s_mtime = 0;
    sb.s_wtime = 0;
    sb.s_mnt_count = 0;
    sb.s_max_mnt_count = 32;
    sb.s_magic = EXT2_SUPER_MAGIC;
    sb.s_state = 1;   /* Clean */
    sb.s_errors = 1;  /* Continue on error */
    sb.s_minor_rev_level = 0;
    sb.s_lastcheck = 0;
    sb.s_checkinterval = 0;
    sb.s_creator_os = 0;  /* Linux */
    sb.s_rev_level = EXT2_DYNAMIC_REV;
    sb.s_def_resuid = 0;
    sb.s_def_resgid = 0;
    sb.s_first_ino = 11;
    sb.s_inode_size = inode_size;

    memcpy(buf + sb_offset, &sb, sizeof(sb));

    /* Write superblock to device */
    if (blockdev_write_sectors(dev_id, sb_block * (block_size / 512),
                               block_size / 512, buf) < 0) {
        kprintf("mkfs.ext2: failed to write superblock\n");
        free(buf);
        return 1;
    }
    kprintf("  Superblock written at block %u\n", sb_block);

    /* ── Write block group descriptors ────────────────────────────── */
    uint32_t bgd_block = (block_size == 1024) ? 2 : 1;
    uint32_t bgd_size = num_groups * sizeof(struct ext2_bg_desc);
    uint32_t bgd_blocks = div_round_up(bgd_size, block_size);

    struct ext2_bg_desc *bgd_table = (struct ext2_bg_desc *)malloc(bgd_blocks * block_size);
    if (!bgd_table) {
        kprintf("mkfs.ext2: out of memory for BGD table\n");
        free(buf);
        return 1;
    }
    memset(bgd_table, 0, bgd_blocks * block_size);

    uint32_t next_block = sb_block + 1 + bgd_blocks;
    for (uint32_t g = 0; g < num_groups; g++) {
        struct ext2_bg_desc *bgd = &bgd_table[g];
        uint32_t group_blocks = (g == num_groups - 1)
            ? block_count - g * blocks_per_group
            : blocks_per_group;

        bgd->bg_block_bitmap = next_block++;
        bgd->bg_inode_bitmap = next_block++;
        bgd->bg_inode_table = next_block;

        uint32_t inode_table_blocks = div_round_up(inodes_per_group * inode_size, block_size);
        next_block += inode_table_blocks;

        bgd->bg_free_blocks_count = group_blocks - (next_block - bgd->bg_inode_table);
        bgd->bg_free_inodes_count = inodes_per_group;
        bgd->bg_used_dirs_count = (g == 0) ? 1 : 0;  /* Root dir in group 0 */
    }

    /* Write BGD table */
    for (uint32_t i = 0; i < bgd_blocks; i++) {
        if (blockdev_write_sectors(dev_id,
                                   (bgd_block + i) * (block_size / 512),
                                   block_size / 512,
                                   (uint8_t *)bgd_table + i * block_size) < 0) {
            kprintf("mkfs.ext2: failed to write BGD block %u\n", i);
            free(bgd_table);
            free(buf);
            return 1;
        }
    }
    kprintf("  BGD table written (%u blocks)\n", bgd_blocks);

    /* ── Write block bitmap (all blocks free, mark metadata blocks in use) ── */
    uint32_t used_blocks = next_block;

    for (uint32_t g = 0; g < num_groups; g++) {
        memset(buf, 0, block_size);
        uint32_t group_start = g * blocks_per_group;
        uint32_t group_blocks = (g == num_groups - 1)
            ? block_count - group_start
            : blocks_per_group;

        if (g == 0) {
            /* Mark all metadata blocks up to used_blocks as used */
            for (uint32_t b = 0; b < used_blocks && b < group_blocks; b++) {
                buf[b / 8] |= (1 << (b % 8));
            }
        }

        uint32_t bitmap_block = bgd_table[g].bg_block_bitmap;
        if (blockdev_write_sectors(dev_id,
                                   bitmap_block * (block_size / 512),
                                   block_size / 512, buf) < 0) {
            kprintf("mkfs.ext2: failed to write block bitmap for group %u\n", g);
            free(bgd_table);
            free(buf);
            return 1;
        }
    }
    kprintf("  Block bitmaps written\n");

    /* ── Write inode bitmap (all free, mark root inode in group 0) ── */
    memset(buf, 0, block_size);
    buf[ROOT_INODE / 8] |= (1 << (ROOT_INODE % 8));  /* Mark inode 2 as used */
    /* Inode 1 is reserved for bad blocks */
    buf[0] |= 0x02;  /* Mark inode 1 as used */

    uint32_t inode_bitmap_block = bgd_table[0].bg_inode_bitmap;
    if (blockdev_write_sectors(dev_id,
                               inode_bitmap_block * (block_size / 512),
                               block_size / 512, buf) < 0) {
        kprintf("mkfs.ext2: failed to write inode bitmap\n");
        free(bgd_table);
        free(buf);
        return 1;
    }
    kprintf("  Inode bitmap written\n");

    /* ── Write root inode ─────────────────────────────────────────── */
    memset(buf, 0, block_size);
    struct ext2_inode *root_inode = (struct ext2_inode *)buf;

    root_inode->i_mode = EXT2_S_IFDIR | EXT2_S_IRWXU | EXT2_S_IRWXG | EXT2_S_IRWXO;
    root_inode->i_uid = 0;
    root_inode->i_size = block_size;  /* One block for root directory */
    root_inode->i_atime = 0;
    root_inode->i_ctime = 0;
    root_inode->i_mtime = 0;
    root_inode->i_dtime = 0;
    root_inode->i_gid = 0;
    root_inode->i_links_count = 2;  /* . and .. */
    root_inode->i_blocks = block_size / 512;  /* 1 block in 512-byte units */
    root_inode->i_flags = 0;

    /* Root directory data block - allocate first available data block */
    uint32_t root_data_block = used_blocks;
    root_inode->i_block[0] = root_data_block;

    /* Write root inode at position ROOT_INODE in inode table */
    uint32_t inode_table_block = bgd_table[0].bg_inode_table;
    uint32_t inode_offset = (ROOT_INODE - 1) * inode_size;

    /* Write the inode at the appropriate offset within the table */
    uint8_t *inode_buf = (uint8_t *)malloc(block_size);
    if (!inode_buf) {
        free(bgd_table);
        free(buf);
        return 1;
    }
    memset(inode_buf, 0, block_size);

    /* Read the inode table block, write the inode, write it back */
    uint32_t inode_block = inode_table_block + inode_offset / block_size;
    uint32_t inode_block_offset = inode_offset % block_size;

    if (blockdev_read_sectors(dev_id,
                              inode_block * (block_size / 512),
                              block_size / 512, inode_buf) < 0) {
        kprintf("mkfs.ext2: failed to read inode table block\n");
        free(inode_buf);
        free(bgd_table);
        free(buf);
        return 1;
    }
    memcpy(inode_buf + inode_block_offset, root_inode, sizeof(struct ext2_inode));
    if (blockdev_write_sectors(dev_id,
                               inode_block * (block_size / 512),
                               block_size / 512, inode_buf) < 0) {
        kprintf("mkfs.ext2: failed to write root inode\n");
        free(inode_buf);
        free(bgd_table);
        free(buf);
        return 1;
    }
    free(inode_buf);
    kprintf("  Root inode written at block %u\n", inode_block);

    /* ── Write root directory entries (. and ..) ──────────────────── */
    memset(buf, 0, block_size);
    uint8_t *dent = buf;

    /* Entry for "." */
    struct ext2_dirent {
        uint32_t inode;
        uint16_t rec_len;
        uint8_t  name_len;
        uint8_t  file_type;
        char     name[4];
    } __attribute__((packed));

    struct ext2_dirent *de = (struct ext2_dirent *)dent;
    de->inode = ROOT_INODE;
    de->rec_len = 12 + 1;  /* 12 bytes header + 1 byte name, padded to 4 */
    /* Pad to 4-byte boundary: 12 + 2 = 14, next multiple of 4 = 16? No... */
    de->rec_len = 12 + 1;
    /* Round up to alignment */
    de->rec_len += (4 - (de->rec_len % 4)) % 4;
    de->name_len = 1;
    de->file_type = 2;  /* DT_DIR */
    de->name[0] = '.';

    /* Entry for ".." */
    de = (struct ext2_dirent *)(dent + de->rec_len);
    de->inode = ROOT_INODE;
    de->rec_len = block_size - ((uint8_t*)de - buf);  /* Last entry fills remainder */
    de->name_len = 2;
    de->file_type = 2;
    de->name[0] = '.';
    de->name[1] = '.';

    if (blockdev_write_sectors(dev_id,
                               root_data_block * (block_size / 512),
                               block_size / 512, buf) < 0) {
        kprintf("mkfs.ext2: failed to write root directory\n");
        free(bgd_table);
        free(buf);
        return 1;
    }
    kprintf("  Root directory written at block %u\n", root_data_block);

    free(bgd_table);
    free(buf);

    kprintf("mkfs.ext2: Done. %u blocks used, %u groups\n", used_blocks, num_groups);
    return 0;
}

/* Init function called at shell startup */
void mkfs_ext2_init(void)
{
    kprintf("[OK] cmd_mkfs_ext2: ext2 filesystem creator ready\n");
}
