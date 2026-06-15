// SPDX-License-Identifier: GPL-2.0-only
/*
 * nilfs2.c — NILFS2 (continuous snapshotting FS) skeleton
 *
 * NILFS2 is a log-structured filesystem that supports continuous
 * snapshotting. This is a minimal skeleton for read support.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "heap.h"

#define NILFS2_SUPER_MAGIC  0x3434
#define NILFS2_BLOCK_SIZE   4096

/* NILFS2 superblock */
struct nilfs2_superblock {
    uint32_t s_rev_level;
    uint16_t s_minor_rev_level;
    uint16_t s_magic;
    uint16_t s_bytes;
    uint16_t s_flags;
    uint32_t s_crc_seed;
    uint32_t s_sum;
    uint32_t s_log_block_size;
    uint64_t s_nsegments;
    uint64_t s_dev_size;
    uint64_t s_first_data_block;
    uint32_t s_blocks_per_segment;
    uint32_t s_r_segments_percentage;
    uint64_t s_last_cno;
    uint64_t s_last_pseg;
    uint64_t s_last_seq;
    uint32_t s_free_blocks_count;
    uint32_t s_ctime;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_state;
    uint16_t s_errors;
    uint64_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_def_resuid;
    uint32_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_dat_entry_size;
    uint16_t s_checkpoint_size;
    uint16_t s_segment_usage_size;
    uint8_t  s_uuid[16];
    uint8_t  s_volume_name[80];
} __attribute__((packed));

static struct nilfs2_superblock nilfs2_sb;
static int nilfs2_mounted;

int nilfs2_mount(const uint8_t *data, uint64_t size)
{
    if (size < sizeof(struct nilfs2_superblock))
        return -EINVAL;

    memcpy(&nilfs2_sb, data, sizeof(struct nilfs2_superblock));

    if (nilfs2_sb.s_magic != NILFS2_SUPER_MAGIC)
        return -EINVAL;

    nilfs2_mounted = 1;
    kprintf("[NILFS2] Mounted: segments=%llu, blocks_per_segment=%u\n",
            (unsigned long long)nilfs2_sb.s_nsegments,
            nilfs2_sb.s_blocks_per_segment);
    return 0;
}

int nilfs2_umount(void)
{
    nilfs2_mounted = 0;
    return 0;
}

int nilfs2_read(uint64_t ino, uint64_t offset, uint8_t *buf, size_t len)
{
    if (!nilfs2_mounted) return -ENODEV;
    (void)ino;
    (void)offset;
    (void)buf;
    (void)len;
    return (int)len;
}

int nilfs2_readdir(uint64_t ino, uint64_t *offset_out,
                    char *name_buf, int *name_len)
{
    if (!nilfs2_mounted) return -ENODEV;
    (void)ino;
    (void)offset_out;
    (void)name_buf;
    (void)name_len;
    return 0;
}

void nilfs2_init(void)
{
    nilfs2_mounted = 0;
    memset(&nilfs2_sb, 0, sizeof(nilfs2_sb));
    kprintf("[OK] NILFS2 — Continuous snapshotting FS skeleton\n");
}
#include "module.h"
module_init(nilfs2_init);
