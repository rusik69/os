/* mkfs_ext2.c — Create ext2 filesystem on device */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Ext2 superblock constants */
#define EXT2_SUPER_MAGIC    0xEF53
#define EXT2_SB_OFFSET      1024
#define EXT2_BLOCK_SIZE     1024
#define EXT2_INODE_SIZE     128
#define EXT2_FIRST_INO      11
#define EXT2_ADDR_PER_BLOCK (EXT2_BLOCK_SIZE / 4)

#define EXT2_S_IFREG  0100000
#define EXT2_S_IFDIR  0040000
#define EXT2_S_IRWXU  00700
#define EXT2_S_IRUSR  00400
#define EXT2_S_IWUSR  00200
#define EXT2_S_IXUSR  00100
#define EXT2_S_IRWXG  00070
#define EXT2_S_IRWXO  00007

/* Superblock structure */
struct ext2_superblock {
    unsigned int  s_inodes_count;
    unsigned int  s_blocks_count;
    unsigned int  s_r_blocks_count;
    unsigned int  s_free_blocks_count;
    unsigned int  s_free_inodes_count;
    unsigned int  s_first_data_block;
    unsigned int  s_log_block_size;
    unsigned int  s_log_frag_size;
    unsigned int  s_blocks_per_group;
    unsigned int  s_frags_per_group;
    unsigned int  s_inodes_per_group;
    unsigned int  s_mtime;
    unsigned int  s_wtime;
    unsigned short s_mnt_count;
    unsigned short s_max_mnt_count;
    unsigned short s_magic;
    unsigned short s_state;
    unsigned short s_errors;
    unsigned short s_minor_rev_level;
    unsigned int  s_lastcheck;
    unsigned int  s_checkinterval;
    unsigned int  s_creator_os;
    unsigned int  s_rev_level;
    unsigned short s_def_resuid;
    unsigned short s_def_resgid;
    /* EXT2_DYNAMIC_REV fields */
    unsigned int  s_first_ino;
    unsigned short s_inode_size;
    unsigned short s_block_group_nr;
    unsigned int  s_feature_compat;
    unsigned int  s_feature_incompat;
    unsigned int  s_feature_ro_compat;
    unsigned char  s_uuid[16];
    char           s_volume_name[16];
    char           s_last_mounted[64];
    unsigned int  s_algorithm_usage_bitmap;
    /* Performance hints */
    unsigned char  s_prealloc_blocks;
    unsigned char  s_prealloc_dir_blocks;
    unsigned short s_padding1;
    /* Journal support */
    unsigned char  s_journal_uuid[16];
    unsigned int  s_journal_inum;
    unsigned int  s_journal_dev;
    unsigned int  s_last_orphan;
    /* Directory indexing */
    unsigned int  s_hash_seed[4];
    unsigned char s_def_hash_version;
    unsigned char s_padding2[3];
    /* Other */
    unsigned int  s_default_mount_options;
    unsigned int  s_first_meta_bg;
    /* Unused */
    unsigned char s_reserved[760];
};

/* Block group descriptor */
struct ext2_group_desc {
    unsigned int  bg_block_bitmap;
    unsigned int  bg_inode_bitmap;
    unsigned int  bg_inode_table;
    unsigned short bg_free_blocks_count;
    unsigned short bg_free_inodes_count;
    unsigned short bg_used_dirs_count;
    unsigned short bg_pad;
    unsigned char  bg_reserved[12];
};

/* Inode structure */
struct ext2_inode {
    unsigned short i_mode;
    unsigned short i_uid;
    unsigned int   i_size;
    unsigned int   i_atime;
    unsigned int   i_ctime;
    unsigned int   i_mtime;
    unsigned int   i_dtime;
    unsigned short i_gid;
    unsigned short i_links_count;
    unsigned int   i_blocks;
    unsigned int   i_flags;
    unsigned int   i_osd1;
    unsigned int   i_block[15];
    unsigned int   i_generation;
    unsigned int   i_file_acl;
    unsigned int   i_dir_acl;
    unsigned int   i_faddr;
    unsigned char  i_osd2[12];
};

/* Directory entry */
struct ext2_dir_entry {
    unsigned int  inode;
    unsigned short rec_len;
    unsigned char  name_len;
    unsigned char  file_type;
    char           name[];
};

static void write_le32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void write_le16(unsigned char *p, unsigned short v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: mkfs_ext2 DEVICE [BLOCK_SIZE]\n");
        return 1;
    }

    const char *device = argv[1];
    int block_size = EXT2_BLOCK_SIZE;

    int fd = open(device, O_RDWR, 0);
    if (fd < 0) {
        printf("mkfs_ext2: cannot open '%s'\n", device);
        return 1;
    }

    /* Determine device size */
    long long end = lseek(fd, 0, SEEK_END);
    if (end <= 0) {
        printf("mkfs_ext2: cannot determine device size\n");
        close(fd);
        return 1;
    }
    lseek(fd, 0, SEEK_SET);

    unsigned long long total_size = (unsigned long long)end;
    unsigned int block_count = (unsigned int)(total_size / block_size);
    unsigned int inodes_per_group = 128;
    unsigned int blocks_per_group = 8192;

    /* Calculate number of block groups */
    unsigned int num_groups = (block_count + blocks_per_group - 1) / blocks_per_group;
    if (num_groups == 0) num_groups = 1;

    unsigned int inodes_count = num_groups * inodes_per_group;

    /* ── Allocate buffers ──────────────────────────────────────── */
    unsigned char block[EXT2_BLOCK_SIZE];
    memset(block, 0, sizeof(block));

    /* ── Write superblock at offset 1024 ────────────────────────── */
    lseek(fd, EXT2_SB_OFFSET, SEEK_SET);

    /* Manually pack superblock fields */
    /* s_inodes_count */
    write_le32(block + 0, inodes_count);
    /* s_blocks_count */
    write_le32(block + 4, block_count);
    /* s_r_blocks_count */
    write_le32(block + 8, 0);
    /* s_free_blocks_count */
    write_le32(block + 12, block_count - 1);  /* minus superblock group */
    /* s_free_inodes_count */
    write_le32(block + 16, inodes_count - 2);  /* minus root + lost+found */
    /* s_first_data_block */
    write_le32(block + 20, 1);
    /* s_log_block_size */
    write_le32(block + 24, 0);  /* 0 = 1024 bytes */
    /* s_log_frag_size */
    write_le32(block + 28, 0);
    /* s_blocks_per_group */
    write_le32(block + 32, blocks_per_group);
    /* s_frags_per_group */
    write_le32(block + 36, blocks_per_group);
    /* s_inodes_per_group */
    write_le32(block + 40, inodes_per_group);
    /* s_mtime */
    write_le32(block + 44, 0);
    /* s_wtime */
    write_le32(block + 48, 0);
    /* s_mnt_count */
    write_le16(block + 52, 0);
    /* s_max_mnt_count */
    write_le16(block + 54, 20);
    /* s_magic */
    write_le16(block + 56, EXT2_SUPER_MAGIC);
    /* s_state */
    write_le16(block + 58, 1);  /* clean */
    /* s_errors */
    write_le16(block + 60, 1);  /* continue */
    /* s_minor_rev_level */
    write_le16(block + 62, 0);
    /* s_lastcheck */
    write_le32(block + 64, 0);
    /* s_checkinterval */
    write_le32(block + 68, 0);
    /* s_creator_os */
    write_le32(block + 72, 0);  /* Linux */
    /* s_rev_level */
    write_le32(block + 76, 1);  /* dynamic rev */
    /* s_def_resuid */
    write_le16(block + 80, 0);
    /* s_def_resgid */
    write_le16(block + 82, 0);
    /* s_first_ino */
    write_le32(block + 84, EXT2_FIRST_INO);
    /* s_inode_size */
    write_le16(block + 88, EXT2_INODE_SIZE);
    /* s_block_group_nr */
    write_le16(block + 90, 0);
    /* s_feature_compat */
    write_le32(block + 92, 0);
    /* s_feature_incompat */
    write_le32(block + 96, 0);
    /* s_feature_ro_compat */
    write_le32(block + 100, 0);
    /* s_uuid (dummy) */
    memset(block + 104, 0, 16);
    /* s_volume_name */
    memset(block + 120, 0, 16);
    memcpy(block + 120, "mkfs_ext2", 9);
    /* s_last_mounted */
    memset(block + 136, 0, 64);

    write(fd, block, sizeof(block));

    /* ── Block group descriptors ────────────────────────────────── */
    /* Block group descriptors start at block 2 (after superblock block 1) */
    lseek(fd, 2 * EXT2_BLOCK_SIZE, SEEK_SET);

    struct ext2_group_desc gd;
    memset(&gd, 0, sizeof(gd));

    if (num_groups == 1) {
        /* Block bitmap at block 3, inode bitmap at block 4, inode table at block 5 */
        gd.bg_block_bitmap = 3;
        gd.bg_inode_bitmap = 4;
        gd.bg_inode_table = 5;
        gd.bg_free_blocks_count = block_count - 5 - (inodes_per_group * EXT2_INODE_SIZE + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;
        gd.bg_free_inodes_count = inodes_per_group - 2;
        gd.bg_used_dirs_count = 1;  /* root directory */
    }

    write(fd, &gd, sizeof(gd));

    /* ── Block bitmap (block 3) ────────────────────────────────── */
    lseek(fd, 3 * EXT2_BLOCK_SIZE, SEEK_SET);
    memset(block, 0, sizeof(block));
    /* Mark first 5 blocks as used (superblock, group desc, block bitmap, inode bitmap, inode table) */
    block[0] = 0x1F;  /* bits 0-4 set = blocks 0-4 used */
    /* Also mark inode table blocks */
    unsigned int inode_blocks = (inodes_per_group * EXT2_INODE_SIZE + EXT2_BLOCK_SIZE - 1) / EXT2_BLOCK_SIZE;
    for (unsigned int i = 5; i < 5 + inode_blocks && i < 8 * sizeof(block); i++) {
        block[i / 8] |= (1 << (i % 8));
    }
    write(fd, block, sizeof(block));

    /* ── Inode bitmap (block 4) ────────────────────────────────── */
    lseek(fd, 4 * EXT2_BLOCK_SIZE, SEEK_SET);
    memset(block, 0, sizeof(block));
    /* Mark inodes 1 (bad blocks) and 2 (root) as used, also 11 (lost+found) */
    block[0] = 0x03;  /* bits 0-1 set */
    block[1] |= 0x08; /* bit 11-1=10? Actually bit 11 is in byte 1, bit 3 */
    write(fd, block, sizeof(block));

    /* ── Inode table (starts at block 5) ───────────────────────── */
    lseek(fd, 5 * EXT2_BLOCK_SIZE, SEEK_SET);

    /* Inode 2: root directory */
    struct ext2_inode root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.i_mode = EXT2_S_IFDIR | 0755;
    root_inode.i_uid = 0;
    root_inode.i_gid = 0;
    root_inode.i_size = EXT2_BLOCK_SIZE;
    root_inode.i_links_count = 2;
    root_inode.i_blocks = 2;  /* in 512-byte units */
    root_inode.i_block[0] = 5 + inode_blocks;  /* first data block after inode table */

    /* Write root inode (inode 2, so 2nd entry) */
    /* Skip inode 1 (bad blocks inode) */
    lseek(fd, 5 * EXT2_BLOCK_SIZE + EXT2_INODE_SIZE, SEEK_SET);
    write(fd, &root_inode, sizeof(root_inode));

    /* ── Root directory data block ──────────────────────────────── */
    unsigned int root_data_block = root_inode.i_block[0];
    lseek(fd, root_data_block * EXT2_BLOCK_SIZE, SEEK_SET);
    memset(block, 0, sizeof(block));

    /* Directory entry: "." */
    struct ext2_dir_entry *de = (struct ext2_dir_entry *)block;
    de->inode = 2;
    de->rec_len = 12;    /* 8 + 4 (name_len=1 padded to 4) */
    de->name_len = 1;
    de->file_type = 2;   /* EXT2_FT_DIR */
    de->name[0] = '.';
    de->name[1] = '\0';
    de->name[2] = '\0';
    de->name[3] = '\0';

    /* Directory entry: ".." */
    de = (struct ext2_dir_entry *)(block + 12);
    de->inode = 2;
    de->rec_len = 12;    /* 8 + 4 */
    de->name_len = 2;
    de->file_type = 2;
    de->name[0] = '.';
    de->name[1] = '.';
    de->name[2] = '\0';
    de->name[3] = '\0';

    /* Directory entry: "lost+found" */
    de = (struct ext2_dir_entry *)(block + 24);
    de->inode = EXT2_FIRST_INO;  /* inode 11 */
    de->rec_len = EXT2_BLOCK_SIZE - 24;  /* remaining space */
    de->name_len = 10;
    de->file_type = 2;
    memcpy(de->name, "lost+found", 10);

    write(fd, block, sizeof(block));

    /* ── lost+found inode (inode 11) ────────────────────────────── */
    lseek(fd, 5 * EXT2_BLOCK_SIZE + (EXT2_FIRST_INO - 1) * EXT2_INODE_SIZE, SEEK_SET);

    struct ext2_inode lf_inode;
    memset(&lf_inode, 0, sizeof(lf_inode));
    lf_inode.i_mode = EXT2_S_IFDIR | 0755;
    lf_inode.i_uid = 0;
    lf_inode.i_gid = 0;
    lf_inode.i_size = EXT2_BLOCK_SIZE;
    lf_inode.i_links_count = 2;
    lf_inode.i_blocks = 2;
    lf_inode.i_block[0] = root_data_block + 1;  /* next data block */

    write(fd, &lf_inode, sizeof(lf_inode));

    /* ── lost+found directory data block ────────────────────────── */
    lseek(fd, (root_data_block + 1) * EXT2_BLOCK_SIZE, SEEK_SET);
    memset(block, 0, sizeof(block));

    de = (struct ext2_dir_entry *)block;
    de->inode = EXT2_FIRST_INO;
    de->rec_len = 12;
    de->name_len = 1;
    de->file_type = 2;
    de->name[0] = '.';

    de = (struct ext2_dir_entry *)(block + 12);
    de->inode = 2;  /* parent is root */
    de->rec_len = EXT2_BLOCK_SIZE - 12;
    de->name_len = 2;
    de->file_type = 2;
    de->name[0] = '.';
    de->name[1] = '.';

    write(fd, block, sizeof(block));

    close(fd);

    printf("mkfs_ext2: ext2 filesystem created on %s\n", device);
    printf("  Blocks: %u, Block groups: %u, Inodes: %u\n",
           block_count, num_groups, inodes_count);
    return 0;
}
