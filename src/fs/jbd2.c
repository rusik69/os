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

/* ── Internal I/O helpers ──────────────────────────────────────────── */

/*
 * Read one journal block (journal->block_size bytes) from the device.
 * Handles the block-size to 512-byte-sector conversion internally.
 */
static int jbd2_read_block(const struct jbd2_journal *journal,
                            uint32_t block_num, uint8_t *buf)
{
    uint32_t sectors_per_block = journal->block_size / 512;
    uint64_t lba = (uint64_t)block_num * sectors_per_block;

    for (uint32_t i = 0; i < sectors_per_block; i++) {
        if (blockdev_read_sectors((int)journal->dev_id,
                                   (uint32_t)(lba + i), 1,
                                   buf + i * 512) != 0)
            return JBD2_ERR_IO;
    }
    return JBD2_OK;
}

/*
 * Write one filesystem block (journal->block_size bytes) to the device.
 * Used during recovery to replay data blocks to their target locations.
 */
static int jbd2_write_fs_block(const struct jbd2_journal *journal,
                                uint32_t block_num, const uint8_t *buf)
{
    uint32_t sectors_per_block = journal->block_size / 512;
    uint64_t lba = (uint64_t)block_num * sectors_per_block;

    for (uint32_t i = 0; i < sectors_per_block; i++) {
        if (blockdev_write_sectors((int)journal->dev_id,
                                    (uint32_t)(lba + i), 1,
                                    buf + i * 512) != 0)
            return JBD2_ERR_IO;
    }
    return JBD2_OK;
}

/* ── Descriptor block processing ───────────────────────────────────── */

/*
 * Process a single descriptor block and replay all its data blocks.
 *
 * Reads the descriptor block from @buf (which was already read at @current),
 * iterates through its block tags, reads the corresponding data blocks
 * from the journal, and writes them to their target filesystem locations.
 *
 * @journal:      initialized journal structure
 * @buf:          buffer containing the descriptor block data
 * @current:      [in/out] on entry: block number of the descriptor;
 *                on exit:  block number after the last data block
 *                (the caller expects to find the commit block here)
 * @block_size:   journal block size in bytes
 *
 * Returns: number of blocks replayed on success,
 *          negative error code on failure.
 */
static int jbd2_replay_descriptor(const struct jbd2_journal *journal,
                                   uint8_t *buf,
                                   uint32_t *current,
                                   uint32_t block_size)
{
    uint32_t data_current;
    int tags_per_block;
    int num_tags;
    uint32_t tag_offset;
    int blocks_replayed;
    int i;

    /* Number of V1 tags that fit in a block */
    tags_per_block = ((int)block_size - (int)sizeof(struct jbd2_header))
                     / (int)sizeof(struct jbd2_block_tag);
    if (tags_per_block <= 0)
        return JBD2_ERR_BAD_MAGIC;

    /* Count tags and find the data block start position */
    num_tags = 0;
    tag_offset = sizeof(struct jbd2_header);
    data_current = *current + 1;

    for (i = 0; i < tags_per_block; i++) {
        const struct jbd2_block_tag *tag;

        if (tag_offset + sizeof(struct jbd2_block_tag) > block_size)
            break;

        tag = (const struct jbd2_block_tag *)(buf + tag_offset);
        num_tags++;
        tag_offset += sizeof(struct jbd2_block_tag);

        if (tag->t_flags & JBD2_FLAG_LAST_TAG)
            break;
    }

    if (num_tags == 0) {
        /* No tags means no data blocks — just advance past header */
        kprintf("[jbd2]  transaction with zero blocks\n");
        *current = data_current;
        return 0;
    }

    kprintf("[jbd2]  descriptor at %u: %d block(s)\n",
            *current, num_tags);

    /* Replay each data block */
    blocks_replayed = 0;
    tag_offset = sizeof(struct jbd2_header);

    for (i = 0; i < num_tags; i++) {
        const struct jbd2_block_tag *tag;
        uint32_t target_block;
        uint32_t data_block_num;
        int ret;

        tag = (const struct jbd2_block_tag *)(buf + tag_offset);
        target_block = tag->t_blocknr;
        data_block_num = data_current;

        /* Handle journal wrap-around */
        if (data_block_num >= journal->total_blocks)
            data_block_num = journal->first_data_block;

        if (!(tag->t_flags & JBD2_FLAG_DELETED)) {
            /* Read data block from the journal */
            ret = jbd2_read_block(journal, data_block_num, buf);
            if (ret != JBD2_OK) {
                kprintf("[jbd2]  I/O error reading journal "
                        "data block %u\n", data_block_num);
                return ret;
            }

            /* Handle escaped blocks:
             * If the file system data block happened to contain the
             * JBD2 magic number, it was replaced with the magic + 0
             * block type during commit.  Restore the original magic. */
            if (tag->t_flags & JBD2_FLAG_ESCAPE) {
                struct jbd2_header *data_hdr =
                    (struct jbd2_header *)buf;
                if (data_hdr->h_magic != JBD2_MAGIC_NUMBER) {
                    /* The original block was replaced with a
                     * JBD2_MAGIC_NUMBER + blocktype==0 block.
                     * Restore the magic that was overwritten. */
                    data_hdr->h_magic = JBD2_MAGIC_NUMBER;
                }
            }

            /* Write the data block to its target filesystem location */
            ret = jbd2_write_fs_block(journal, target_block, buf);
            if (ret != JBD2_OK) {
                kprintf("[jbd2]  failed to write block %u "
                        "during recovery\n", target_block);
                return ret;
            }

            blocks_replayed++;
        }

        data_current++;
        tag_offset += sizeof(struct jbd2_block_tag);
    }

    /* Advance past all data blocks */
    *current = data_current;
    return blocks_replayed;
}

/* ── Journal clean marking ─────────────────────────────────────────── */

/*
 * Mark the journal as clean by updating the superblock in place.
 *
 * Sets s_start to 0xFFFFFFFF (clean marker) and advances
 * s_sequence past the last replayed transaction.
 */
static int jbd2_clean_journal(struct jbd2_journal *journal,
                               uint32_t next_sequence)
{
    uint8_t *buf;
    struct jbd2_superblock *sb;
    int ret;

    buf = (uint8_t *)kmalloc(JBD2_SUPERBLOCK_SIZE);
    if (!buf)
        return -ENOMEM;

    memset(buf, 0, JBD2_SUPERBLOCK_SIZE);

    /* Read the current superblock */
    ret = jbd2_read_block(journal, 0, buf);
    if (ret != JBD2_OK) {
        kprintf("[jbd2] failed to read superblock for "
                "clean marking\n");
        kfree(buf);
        return ret;
    }

    sb = (struct jbd2_superblock *)buf;

    /* Validate it's really our superblock */
    if (sb->s_header.h_magic != JBD2_MAGIC_NUMBER) {
        kprintf("[jbd2] invalid superblock during clean marking\n");
        kfree(buf);
        return JBD2_ERR_BAD_MAGIC;
    }

    /* Update fields */
    journal->start_block = 0xFFFFFFFF;
    journal->sequence = next_sequence;
    journal->errno_val = 0;

    sb->s_start = 0xFFFFFFFF;
    sb->s_sequence = next_sequence;
    sb->s_errno = 0;

    /* Write the updated superblock back */
    ret = jbd2_write_fs_block(journal, 0, buf);
    if (ret != JBD2_OK) {
        kprintf("[jbd2] failed to write clean superblock\n");
        kfree(buf);
        return ret;
    }

    kfree(buf);
    kprintf("[jbd2] journal marked clean: sequence=%u\n", next_sequence);
    return JBD2_OK;
}

/* ── Journal recovery (replay) ────────────────────────────────────── */

int jbd2_replay(struct jbd2_journal *journal)
{
    uint8_t *buf;
    uint32_t current;
    uint32_t block_size;
    int transactions;
    int ret;

    if (!journal)
        return -EINVAL;

    /* Check if there's anything to recover */
    ret = jbd2_get_state(journal);
    if (ret != JBD2_STATE_DIRTY) {
        if (ret == JBD2_STATE_ERROR) {
            kprintf("[jbd2] journal has recorded error %u, "
                    "manual fsck required\n", journal->errno_val);
            return JBD2_ERR_BAD_MAGIC;
        }
        kprintf("[jbd2] journal is clean, no recovery needed\n");
        return 0;
    }

    block_size = journal->block_size;
    if (block_size == 0 || block_size > JBD2_MAX_BLOCK_SIZE) {
        kprintf("[jbd2] invalid block size %u for recovery\n",
                block_size);
        return JBD2_ERR_BLOCK_SIZE;
    }

    buf = (uint8_t *)kmalloc(block_size);
    if (!buf)
        return -ENOMEM;

    current = journal->start_block;
    transactions = 0;

    kprintf("[jbd2] journal recovery: start_block=%u, "
            "first_data=%u, total=%u, seq=%u\n",
            current, journal->first_data_block,
            journal->total_blocks, journal->sequence);

    /* Scan the journal for committed transactions */
    while (1) {
        const struct jbd2_header *hdr;
        uint32_t expected_seq;

        /* Handle circular wrap-around */
        if (current >= journal->total_blocks)
            current = journal->first_data_block;

        /* Read the next journal block */
        ret = jbd2_read_block(journal, current, buf);
        if (ret != JBD2_OK) {
            kprintf("[jbd2] I/O error reading block %u "
                    "during recovery\n", current);
            goto out_err;
        }

        hdr = (const struct jbd2_header *)buf;

        /* No magic number means end of recoverable data */
        if (hdr->h_magic != JBD2_MAGIC_NUMBER) {
            kprintf("[jbd2] no magic at block %u "
                    "(recovered %d transactions)\n",
                    current, transactions);
            break;
        }

        /* Hit the superblock again → wrapped completely around */
        if (hdr->h_blocktype == JBD2_SUPERBLOCK_V1 ||
            hdr->h_blocktype == JBD2_SUPERBLOCK_V2) {
            kprintf("[jbd2] reached superblock at block %u "
                    "(recovered %d transactions)\n",
                    current, transactions);
            break;
        }

        /* Skip orphan commit/revocation blocks */
        if (hdr->h_blocktype == JBD2_COMMIT_BLOCK) {
            current++;
            continue;
        }
        if (hdr->h_blocktype == JBD2_REVOKE_BLOCK) {
            current++;
            continue;
        }

        /* Must be a descriptor block to continue */
        if (hdr->h_blocktype != JBD2_DESCRIPTOR_BLOCK) {
            kprintf("[jbd2] unknown block type %u at %u "
                    "(recovered %d transactions)\n",
                    hdr->h_blocktype, current, transactions);
            break;
        }

        /* Verify sequence number is contiguous */
        expected_seq = journal->sequence + transactions;
        if (hdr->h_sequence != expected_seq) {
            kprintf("[jbd2] sequence mismatch at block %u: "
                    "expected %u, got %u\n",
                    current, expected_seq, hdr->h_sequence);
            break;
        }

        /* Replay this descriptor's data blocks */
        ret = jbd2_replay_descriptor(journal, buf,
                                      &current, block_size);
        if (ret < 0)
            goto out_err;

        /* Verify the commit block exists */
        if (current >= journal->total_blocks)
            current = journal->first_data_block;

        ret = jbd2_read_block(journal, current, buf);
        if (ret != JBD2_OK) {
            kprintf("[jbd2] I/O error reading commit block "
                    "at %u\n", current);
            goto out_err;
        }

        hdr = (const struct jbd2_header *)buf;
        if (hdr->h_magic != JBD2_MAGIC_NUMBER ||
            hdr->h_blocktype != JBD2_COMMIT_BLOCK) {
            kprintf("[jbd2] missing commit block for seq %u "
                    "at block %u\n", expected_seq, current);
            ret = JBD2_ERR_BAD_MAGIC;
            goto out_err;
        }

        /* Advance past the commit block */
        current++;
        transactions++;

        kprintf("[jbd2]  transaction seq=%u committed, "
                "%d block(s) replayed\n",
                expected_seq, ret);
    }

    if (transactions > 0) {
        /* Mark the journal clean */
        ret = jbd2_clean_journal(journal,
                                  journal->sequence + transactions);
        if (ret != JBD2_OK) {
            kprintf("[jbd2] failed to mark journal clean\n");
            goto out_err;
        }
    }

    kfree(buf);
    kprintf("[jbd2] journal recovery complete: %d transaction(s) "
            "replayed\n", transactions);
    return transactions;

out_err:
    kfree(buf);
    return ret;
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
