/*
 * src/fs/jbd2.c — JBD2 (Journaling Block Device 2) superblock parsing.
 *
 * Implements parsing and validation of the JBD2 journal superblock,
 * which is stored at block 0 of the journal device or journal inode.
 * The superblock describes the journal geometry (block size, total
 * blocks, first data block, transaction sequence numbers) and feature
 * flags.  This is the entry point for ext4 journal recovery.
 *
 * Conforms to the Linux JBD2 v2 on-disk format for compatibility with
 * ext4 filesystems created by mkfs.ext4.
 *
 * Part of D177: ext4 journaling support (Task 5: JBD2 journal superblock
 * parsing).
 */

#define KERNEL_INTERNAL
#include "jbd2.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "initcall.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Superblock validation ─────────────────────────────────────────── */

int jbd2_check_superblock(const struct jbd2_superblock *sb,
                           uint32_t block_size)
{
    if (!sb)
        return -EINVAL;

    /* Check magic number */
    if (sb->s_header.h_magic != JBD2_MAGIC_NUMBER) {
        kprintf("[jbd2] bad magic: 0x%08x (expected 0x%08x)\n",
                sb->s_header.h_magic, JBD2_MAGIC_NUMBER);
        return JBD2_ERR_BAD_MAGIC;
    }

    /* Check block type — must be SUPERBLOCK_V2 or SUPERBLOCK_V1 */
    if (sb->s_header.h_blocktype != JBD2_SUPERBLOCK_V2 &&
        sb->s_header.h_blocktype != JBD2_SUPERBLOCK_V1) {
        kprintf("[jbd2] unexpected block type: %u (expected %u or %u)\n",
                sb->s_header.h_blocktype,
                JBD2_SUPERBLOCK_V1, JBD2_SUPERBLOCK_V2);
        return JBD2_ERR_BAD_MAGIC;
    }

    /* Validate block size */
    uint32_t jbd_block_size = sb->s_blocksize;
    if (jbd_block_size == 0) {
        kprintf("[jbd2] zero block size in superblock\n");
        return JBD2_ERR_BLOCK_SIZE;
    }

    if (jbd_block_size > JBD2_MAX_BLOCK_SIZE) {
        kprintf("[jbd2] block size %u exceeds maximum %u\n",
                jbd_block_size, JBD2_MAX_BLOCK_SIZE);
        return JBD2_ERR_BLOCK_SIZE;
    }

    if (jbd_block_size < 512) {
        kprintf("[jbd2] block size %u too small (minimum 512)\n",
                jbd_block_size);
        return JBD2_ERR_BLOCK_SIZE;
    }

    /* Validate against filesystem block size if provided */
    if (block_size != 0 && jbd_block_size != block_size) {
        kprintf("[jbd2] block size mismatch: journal %u, fs %u\n",
                jbd_block_size, block_size);
        return JBD2_ERR_BLOCK_SIZE;
    }

    /* Validate total blocks (s_maxlen) */
    if (sb->s_maxlen < JBD2_MIN_BLOCKS) {
        kprintf("[jbd2] journal too small: %u blocks (minimum %u)\n",
                sb->s_maxlen, JBD2_MIN_BLOCKS);
        return JBD2_ERR_BLOCK_SIZE;
    }

    /* Validate first data block offset */
    if (sb->s_first < 1) {
        kprintf("[jbd2] first data block less than 1: %u\n",
                sb->s_first);
        return JBD2_ERR_BAD_MAGIC;
    }

    if (sb->s_first >= sb->s_maxlen) {
        kprintf("[jbd2] first data block %u beyond maxlen %u\n",
                sb->s_first, sb->s_maxlen);
        return JBD2_ERR_BAD_MAGIC;
    }

    /* Validate start block (0xFFFFFFFF means no transactions) */
    if (sb->s_start != 0xFFFFFFFF && sb->s_start >= sb->s_maxlen) {
        kprintf("[jbd2] start block %u exceeds maxlen %u\n",
                sb->s_start, sb->s_maxlen);
        return JBD2_ERR_BAD_MAGIC;
    }

    return JBD2_OK;
}

/* ── Load journal superblock from device ────────────────────────────── */

int jbd2_load_superblock(struct jbd2_journal *journal, uint8_t dev_id,
                          uint32_t journal_inum, uint32_t block_size)
{
    int ret;

    if (!journal)
        return -EINVAL;

    memset(journal, 0, sizeof(*journal));

    /* Allocate a buffer large enough for the superblock.
     * We use the maximum supported block size to be safe. */
    uint8_t *buf = (uint8_t *)kmalloc(JBD2_MAX_BLOCK_SIZE);
    if (!buf)
        return -ENOMEM;

    memset(buf, 0, JBD2_MAX_BLOCK_SIZE);

    if (journal_inum == 0) {
        /* External journal device — superblock is at block 0.
         * Read the first sector(s) containing the superblock. */
        if (blockdev_read_sectors((int)dev_id, 0, 1, buf) != 0) {
            kprintf("[jbd2] failed to read block 0 from device %u\n",
                    dev_id);
            kfree(buf);
            return JBD2_ERR_IO;
        }
    } else {
        /* Inode-based journal (embedded inside the filesystem).
         * The journal inode's first block contains the superblock.
         * For now, read directly from block 0 of the device and
         * check if the magic is present.
         *
         * TODO: When ext4 gains write support, resolve the journal
         * inode's i_block[] to find the correct physical block.
         * For read-only mounting, the superblock is at block 0 of
         * the device for external journals; for embedded journals
         * we would need to parse the journal inode extent tree. */
        if (blockdev_read_sectors((int)dev_id, 0, 1, buf) != 0) {
            kprintf("[jbd2] failed to read block 0 from device %u\n",
                    dev_id);
            kfree(buf);
            return JBD2_ERR_IO;
        }

        /* Tell the user we're guessing at block 0 for now */
        kprintf("[jbd2] warning: inode-based journal detected (inum=%u),\n"
                "         reading block 0 as tentative superblock\n",
                journal_inum);
    }

    /* Parse the superblock */
    struct jbd2_superblock *sb = (struct jbd2_superblock *)buf;

    ret = jbd2_check_superblock(sb, block_size);
    if (ret != JBD2_OK) {
        kfree(buf);
        return ret;
    }

    /* Copy parsed data to in-memory journal structure */
    journal->dev_id = dev_id;
    journal->inum = journal_inum;
    journal->block_size = sb->s_blocksize;
    journal->total_blocks = sb->s_maxlen;
    journal->first_data_block = sb->s_first;
    journal->sequence = sb->s_sequence;
    journal->start_block = sb->s_start;
    journal->errno_val = sb->s_errno;
    journal->compat = sb->s_feature_compat;
    journal->incompat = sb->s_feature_incompat;
    journal->ro_compat = sb->s_feature_ro_compat;
    memcpy(journal->uuid, sb->s_uuid, 16);

    /* Check journal error state */
    if (sb->s_errno != 0) {
        kprintf("[jbd2] journal has recorded error %u, "
                "may require fsck\n", sb->s_errno);
    }

    /* Check for incompatible features we don't support */
    if (sb->s_feature_incompat & ~(JBD2_FEATURE_INCOMPAT_REVOKE |
                                    JBD2_FEATURE_INCOMPAT_64BIT)) {
        kprintf("[jbd2] warning: unsupported incompat features: 0x%08x\n",
                sb->s_feature_incompat);
    }

    kprintf("[jbd2] journal loaded: dev=%u, inum=%u, "
            "blocks=%u, blk_size=%u, first=%u, seq=%u, start=%u\n",
            dev_id, journal_inum,
            journal->total_blocks, journal->block_size,
            journal->first_data_block, journal->sequence,
            journal->start_block);

    kfree(buf);
    return JBD2_OK;
}

/* ── Journal state determination ────────────────────────────────────── */

int jbd2_get_state(const struct jbd2_journal *journal)
{
    if (!journal)
        return JBD2_STATE_ERROR;

    /* If the journal recorded an error, it needs fsck */
    if (journal->errno_val != 0)
        return JBD2_STATE_ERROR;

    /* If start == 0xFFFFFFFF, the journal is empty/clean */
    if (journal->start_block == 0xFFFFFFFF)
        return JBD2_STATE_CLEAN;

    /* If start == 0, the journal is clean (no transactions to replay) */
    if (journal->start_block == 0)
        return JBD2_STATE_CLEAN;

    /* If start >= first_data_block, there are transactions to replay */
    if (journal->start_block >= journal->first_data_block)
        return JBD2_STATE_DIRTY;

    /* Default to clean (no identifiable transaction data) */
    return JBD2_STATE_CLEAN;
}

/* ── Module init ───────────────────────────────────────────────────── */

int __init jbd2_init(void)
{
    kprintf("[jbd2] JBD2 journal block device module initialized\n");
    return 0;
}

device_initcall(jbd2_init);

#ifdef MODULE
int __init init_module(void) { return jbd2_init(); }
void __exit cleanup_module(void) {}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("JBD2 - Journaling Block Device v2 for ext4");
MODULE_VERSION("1.0");
#endif
