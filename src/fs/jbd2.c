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
 * Forward declarations for static helpers defined later in this file.
 */
static int jbd2_read_block(const struct jbd2_journal *journal,
                            uint32_t block_num, uint8_t *buf);
static int jbd2_write_fs_block(const struct jbd2_journal *journal,
                                uint32_t block_num, const uint8_t *buf);

/*
 * Write one journal block (journal->block_size bytes) to the journal
 * area of the device.  The journal occupies blocks 0 through
 * journal->total_blocks - 1.
 */
static int jbd2_write_journal_block(const struct jbd2_journal *journal,
                                     uint32_t block_num,
                                     const uint8_t *buf)
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

/* ── Transaction commit API ────────────────────────────────────────── */

struct jbd2_handle *jbd2_journal_start(struct jbd2_journal *journal,
                                        uint32_t max_blocks)
{
    struct jbd2_handle *handle;

    if (!journal || max_blocks == 0)
        return NULL;

    /* Clamp to a reasonable maximum */
    if (max_blocks > 2048)
        max_blocks = 2048;

    handle = (struct jbd2_handle *)kmalloc(sizeof(*handle));
    if (!handle)
        return NULL;

    memset(handle, 0, sizeof(*handle));

    /* Allocate parallel arrays for block tracking */
    handle->h_fs_blocknrs = (uint32_t *)kmalloc(
        max_blocks * sizeof(uint32_t));
    if (!handle->h_fs_blocknrs) {
        kfree(handle);
        return NULL;
    }

    handle->h_data = (uint8_t **)kmalloc(
        max_blocks * sizeof(uint8_t *));
    if (!handle->h_data) {
        kfree(handle->h_fs_blocknrs);
        kfree(handle);
        return NULL;
    }

    /* Allocate revocation tracking array — same initial capacity */
    handle->h_revoke_blocks = (uint32_t *)kmalloc(
        max_blocks * sizeof(uint32_t));
    if (!handle->h_revoke_blocks) {
        kfree(handle->h_data);
        kfree(handle->h_fs_blocknrs);
        kfree(handle);
        return NULL;
    }

    memset(handle->h_fs_blocknrs, 0, max_blocks * sizeof(uint32_t));
    memset(handle->h_data, 0, max_blocks * sizeof(uint8_t *));
    memset(handle->h_revoke_blocks, 0, max_blocks * sizeof(uint32_t));

    handle->h_journal = journal;
    handle->h_sequence = journal->sequence;
    handle->h_num_blocks = 0;
    handle->h_capacity = max_blocks;
    handle->h_revoke_count = 0;
    handle->h_revoke_capacity = max_blocks;
    handle->h_state = JBD2_T_STATE_ACTIVE;

    kprintf("[jbd2]  transaction seq=%u started (max %u blocks, %u revoke slots)\n",
            handle->h_sequence, max_blocks, max_blocks);

    return handle;
}

int jbd2_journal_get_write_access(struct jbd2_handle *handle,
                                   uint32_t fs_blocknr,
                                   const uint8_t *data)
{
    uint8_t *block_copy;

    if (!handle || !data)
        return -EINVAL;

    if (handle->h_state != JBD2_T_STATE_ACTIVE)
        return -EINVAL;

    if (handle->h_num_blocks >= handle->h_capacity)
        return -ENOSPC;

    /* Allocate and copy the block data */
    block_copy = (uint8_t *)kmalloc(handle->h_journal->block_size);
    if (!block_copy)
        return -ENOMEM;

    memcpy(block_copy, data, handle->h_journal->block_size);

    /* Register in the handle */
    handle->h_fs_blocknrs[handle->h_num_blocks] = fs_blocknr;
    handle->h_data[handle->h_num_blocks] = block_copy;
    handle->h_num_blocks++;

    return 0;
}

int jbd2_journal_dirty_metadata(struct jbd2_handle *handle,
                                 uint32_t fs_blocknr,
                                 const uint8_t *data)
{
    uint32_t i;

    if (!handle || !data)
        return -EINVAL;

    if (handle->h_state != JBD2_T_STATE_ACTIVE)
        return -EINVAL;

    /* Find the previously-registered block and update its data */
    for (i = 0; i < handle->h_num_blocks; i++) {
        if (handle->h_fs_blocknrs[i] == fs_blocknr) {
            memcpy(handle->h_data[i], data,
                   handle->h_journal->block_size);
            return 0;
        }
    }

    /* Block not found — register it as a new block */
    return jbd2_journal_get_write_access(handle, fs_blocknr, data);
}

void jbd2_journal_stop(struct jbd2_handle *handle)
{
    uint32_t i;

    if (!handle)
        return;

    /* Free any data blocks that were allocated */
    for (i = 0; i < handle->h_num_blocks; i++) {
        if (handle->h_data[i])
            kfree(handle->h_data[i]);
    }

    if (handle->h_fs_blocknrs)
        kfree(handle->h_fs_blocknrs);

    if (handle->h_data)
        kfree(handle->h_data);

    if (handle->h_revoke_blocks)
        kfree(handle->h_revoke_blocks);

    handle->h_state = JBD2_T_STATE_DONE;
    kfree(handle);
}

/* ── Revocation API ────────────────────────────────────────────────── */

int jbd2_journal_revoke(struct jbd2_handle *handle, uint32_t fs_blocknr)
{
    uint32_t i;

    if (!handle)
        return -EINVAL;

    if (handle->h_state != JBD2_T_STATE_ACTIVE)
        return -EINVAL;

    /* Check if already revoked */
    for (i = 0; i < handle->h_revoke_count; i++) {
        if (handle->h_revoke_blocks[i] == fs_blocknr)
            return 0;  /* Already revoked, not an error */
    }

    /* Check capacity, extend if needed */
    if (handle->h_revoke_count >= handle->h_revoke_capacity) {
        uint32_t new_capacity;
        uint32_t *new_blocks;

        new_capacity = handle->h_revoke_capacity * 2;
        if (new_capacity < 16)
            new_capacity = 16;

        new_blocks = (uint32_t *)krealloc(handle->h_revoke_blocks,
            new_capacity * sizeof(uint32_t));
        if (!new_blocks)
            return -ENOMEM;

        handle->h_revoke_blocks = new_blocks;
        handle->h_revoke_capacity = new_capacity;
    }

    handle->h_revoke_blocks[handle->h_revoke_count] = fs_blocknr;
    handle->h_revoke_count++;

    return 0;
}

/* ── Transaction descriptor writing ────────────────────────────────── */

/*
 * Write one descriptor block containing as many tags as will fit.
 *
 * @journal:      journal to write to
 * @handle:       transaction handle
 * @start_tag:    index of first tag to write in handle->h_fs_blocknrs[]
 * @num_tags:     number of tags to include
 * @journal_block: block number in the journal to write the descriptor to
 * @data_start:   first block number in the journal for data blocks
 *
 * Returns: 0 on success, negative error code on failure.
 */
static int jbd2_write_descriptor_block(
    const struct jbd2_journal *journal,
    const struct jbd2_handle *handle,
    uint32_t start_tag, uint32_t num_tags,
    uint32_t journal_block, uint32_t data_start)
{
    uint32_t block_size;
    uint8_t *buf;
    struct jbd2_header *hdr;
    struct jbd2_block_tag *tags;
    uint32_t tag_offset;
    uint32_t i;
    int ret;

    block_size = journal->block_size;

    buf = (uint8_t *)kmalloc(block_size);
    if (!buf)
        return -ENOMEM;

    memset(buf, 0, block_size);

    /* Fill in the header */
    hdr = (struct jbd2_header *)buf;
    hdr->h_magic = JBD2_MAGIC_NUMBER;
    hdr->h_blocktype = JBD2_DESCRIPTOR_BLOCK;
    hdr->h_sequence = handle->h_sequence;

    /* Fill in tags */
    tag_offset = sizeof(struct jbd2_header);
    for (i = 0; i < num_tags; i++) {
        if (tag_offset + sizeof(struct jbd2_block_tag) > block_size)
            break;

        tags = (struct jbd2_block_tag *)(buf + tag_offset);
        tags->t_blocknr = handle->h_fs_blocknrs[start_tag + i];
        tags->t_flags = 0;

        tag_offset += sizeof(struct jbd2_block_tag);
    }

    /* Set LAST_TAG on the final tag in this descriptor */
    if (num_tags > 0) {
        tags = (struct jbd2_block_tag *)(
            buf + sizeof(struct jbd2_header)
            + (num_tags - 1) * sizeof(struct jbd2_block_tag));
        tags->t_flags |= JBD2_FLAG_LAST_TAG;
    }

    /* Write the descriptor block */
    ret = jbd2_write_journal_block(journal, journal_block, buf);
    if (ret != JBD2_OK)
        kprintf("[jbd2]  failed to write descriptor block at %u\n",
                journal_block);

    kfree(buf);
    return ret;
}

/* ── Revocation block writing ──────────────────────────────────────── */

/*
 * Write one revocation block for the given handle.
 *
 * A revocation block records filesystem block numbers that should NOT
 * be replayed during journal recovery.  This is necessary when a block
 * was allocated and freed within the same transaction — if we replayed
 * the old data, it would restore stale/corrupt data.
 *
 * @journal:      journal to write to
 * @revoke_blocks: array of block numbers to revoke
 * @revoke_count:  number of entries in the array
 * @journal_block: block number in the journal to write the revocation to
 *
 * Returns: 0 on success, negative error code on failure.
 */
static int jbd2_write_revoke_block(const struct jbd2_journal *journal,
                                    uint32_t sequence,
                                    const uint32_t *revoke_blocks,
                                    uint32_t revoke_count,
                                    uint32_t journal_block)
{
    uint32_t block_size;
    uint8_t *buf;
    struct jbd2_revoke_header *r_hdr;
    uint32_t max_revokes;
    uint32_t i;
    int ret;

    block_size = journal->block_size;

    if (revoke_count == 0 || !revoke_blocks)
        return 0;

    buf = (uint8_t *)kmalloc(block_size);
    if (!buf)
        return -ENOMEM;

    memset(buf, 0, block_size);

    /* Fill in the revocation block header */
    r_hdr = (struct jbd2_revoke_header *)buf;
    r_hdr->r_header.h_magic = JBD2_MAGIC_NUMBER;
    r_hdr->r_header.h_blocktype = JBD2_REVOKE_BLOCK;
    r_hdr->r_header.h_sequence = sequence;

    /* Calculate how many revoke records fit */
    max_revokes = (block_size - (uint32_t)sizeof(struct jbd2_revoke_header))
                  / sizeof(uint32_t);
    if (max_revokes > revoke_count)
        max_revokes = revoke_count;

    /* Write revoke records after the header */
    {
        uint32_t *revokes = (uint32_t *)(buf + sizeof(struct jbd2_revoke_header));
        for (i = 0; i < max_revokes; i++)
            revokes[i] = revoke_blocks[i];
    }

    /* Set the count field (bytes used in revoke data area) */
    r_hdr->r_count = max_revokes * sizeof(uint32_t);

    kprintf("[jbd2]  writing revoke block at %u: %u block(s)\n",
            journal_block, max_revokes);

    ret = jbd2_write_journal_block(journal, journal_block, buf);
    if (ret != JBD2_OK)
        kprintf("[jbd2]  failed to write revoke block at %u\n",
                journal_block);

    kfree(buf);
    return ret;
}

/* ── Transaction commit ────────────────────────────────────────────── */

int jbd2_commit_transaction(struct jbd2_handle *handle)
{
    struct jbd2_journal *journal;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t first_data;
    uint32_t i;
    uint32_t j;
    uint32_t tags_per_block;
    uint32_t num_tags;
    uint32_t desc_blocks;
    uint32_t journal_pos;
    uint32_t data_pos;
    uint32_t start_pos;
    uint8_t *commit_buf;
    struct jbd2_header *commit_hdr;
    int ret;

    if (!handle || !handle->h_journal)
        return -EINVAL;

    if (handle->h_state != JBD2_T_STATE_ACTIVE)
        return -EINVAL;

    if (handle->h_num_blocks == 0) {
        /* No blocks to journal — nothing to commit */
        kprintf("[jbd2]  transaction seq=%u has zero blocks, "
                "nothing to commit\n", handle->h_sequence);
        jbd2_journal_stop(handle);
        return 0;
    }

    journal = handle->h_journal;
    handle->h_state = JBD2_T_STATE_COMMITTING;

    block_size = journal->block_size;
    total_blocks = journal->total_blocks;
    first_data = journal->first_data_block;
    num_tags = handle->h_num_blocks;

    /* Calculate how many descriptor blocks we need */
    tags_per_block = (uint32_t)((block_size - sizeof(struct jbd2_header))
                     / sizeof(struct jbd2_block_tag));
    if (tags_per_block == 0) {
        kprintf("[jbd2]  block size %u too small for tags\n",
                block_size);
        ret = JBD2_ERR_BLOCK_SIZE;
        goto out_err;
    }

    desc_blocks = (num_tags + tags_per_block - 1) / tags_per_block;

    /* Find a free location in the journal.
     * We start writing at the current s_start or first_data_block,
     * whichever is valid.  If the journal tail is at 0xFFFFFFFF (empty),
     * start at first_data_block. */
    if (journal->start_block == 0xFFFFFFFF ||
        journal->start_block == 0)
        journal_pos = first_data;
    else
        journal_pos = journal->start_block;

    /* Move past any already-committed transactions to find free space.
     * For simplicity, we append after the current start if it's valid,
     * wrapping around as needed. */
    start_pos = journal_pos;

    kprintf("[jbd2]  committing seq=%u: %u block(s) at journal "
            "block %u, %u desc block(s)\n",
            handle->h_sequence, num_tags, journal_pos, desc_blocks);

    /* Write descriptor blocks */
    for (i = 0; i < desc_blocks; i++) {
        uint32_t tags_this_desc;
        uint32_t start_tag;
        uint32_t desc_block;

        start_tag = i * tags_per_block;
        tags_this_desc = num_tags - start_tag;
        if (tags_this_desc > tags_per_block)
            tags_this_desc = tags_per_block;

        desc_block = journal_pos;
        data_pos = journal_pos + 1;

        ret = jbd2_write_descriptor_block(
            journal, handle,
            start_tag, tags_this_desc,
            desc_block, data_pos);
        if (ret != JBD2_OK) {
            kprintf("[jbd2]  descriptor write failed at block %u\n",
                    desc_block);
            goto out_err;
        }

        journal_pos++; /* Past the descriptor */

        /* Write data blocks for this descriptor */
        for (j = 0; j < tags_this_desc; j++) {
            uint32_t data_block = journal_pos;
            uint32_t idx = start_tag + j;

            /* Handle circular wrap-around within journal area */
            if (data_block >= total_blocks)
                data_block = first_data;

            if (data_block >= total_blocks) {
                kprintf("[jbd2]  data block %u exceeds journal "
                        "bounds\n", data_block);
                ret = JBD2_ERR_BLOCK_SIZE;
                goto out_err;
            }

            ret = jbd2_write_journal_block(
                journal, data_block,
                handle->h_data[idx]);
            if (ret != JBD2_OK) {
                kprintf("[jbd2]  data block write failed at %u "
                        "(fs block %u)\n",
                        data_block,
                        handle->h_fs_blocknrs[idx]);
                goto out_err;
            }

            journal_pos++;
        }
    }

    /* ── Write revocation blocks (if any) before the commit block ──── */
    if (handle->h_revoke_count > 0) {
        uint32_t revoke_block;

        /* Handle circular wrap-around */
        if (journal_pos >= total_blocks)
            journal_pos = first_data;

        revoke_block = journal_pos;

        kprintf("[jbd2]  writing revoke block at %u: %u block(s)\n",
                revoke_block, handle->h_revoke_count);

        ret = jbd2_write_revoke_block(journal, handle->h_sequence,
                                       handle->h_revoke_blocks,
                                       handle->h_revoke_count,
                                       revoke_block);
        if (ret != JBD2_OK) {
            kprintf("[jbd2]  failed to write revoke block at %u\n",
                    revoke_block);
            goto out_err;
        }

        journal_pos++; /* Past the revoke block */
    }

    /* Write the commit block */
    if (journal_pos >= total_blocks)
        journal_pos = first_data;

    if (journal_pos >= total_blocks) {
        kprintf("[jbd2]  commit block position %u invalid\n",
                journal_pos);
        ret = JBD2_ERR_BLOCK_SIZE;
        goto out_err;
    }

    commit_buf = (uint8_t *)kmalloc(block_size);
    if (!commit_buf) {
        ret = -ENOMEM;
        goto out_err;
    }

    memset(commit_buf, 0, block_size);
    commit_hdr = (struct jbd2_header *)commit_buf;
    commit_hdr->h_magic = JBD2_MAGIC_NUMBER;
    commit_hdr->h_blocktype = JBD2_COMMIT_BLOCK;
    commit_hdr->h_sequence = handle->h_sequence;

    ret = jbd2_write_journal_block(journal, journal_pos, commit_buf);
    kfree(commit_buf);

    if (ret != JBD2_OK) {
        kprintf("[jbd2]  failed to write commit block at %u\n",
                journal_pos);
        goto out_err;
    }

    kprintf("[jbd2]  commit block written at %u\n", journal_pos);

    /* Advance past the commit block for next start position */
    journal_pos++;
    if (journal_pos >= total_blocks)
        journal_pos = first_data;

    /* Update the journal superblock with new start and sequence */
    journal->start_block = start_pos;
    journal->sequence = handle->h_sequence + 1;
    journal->errno_val = 0;

    /* Write the updated superblock */
    {
        uint8_t *sb_buf;
        struct jbd2_superblock *sb;

        sb_buf = (uint8_t *)kmalloc(JBD2_SUPERBLOCK_SIZE);
        if (!sb_buf) {
            ret = -ENOMEM;
            goto out_err;
        }

        memset(sb_buf, 0, JBD2_SUPERBLOCK_SIZE);

        /* Read current superblock */
        ret = jbd2_read_block(journal, 0, sb_buf);
        if (ret != JBD2_OK) {
            kfree(sb_buf);
            kprintf("[jbd2]  failed to read superblock for update\n");
            goto out_err;
        }

        sb = (struct jbd2_superblock *)sb_buf;

        if (sb->s_header.h_magic != JBD2_MAGIC_NUMBER) {
            kfree(sb_buf);
            kprintf("[jbd2]  invalid superblock during commit\n");
            ret = JBD2_ERR_BAD_MAGIC;
            goto out_err;
        }

        /* Update fields */
        sb->s_start = start_pos;
        sb->s_sequence = handle->h_sequence + 1;
        sb->s_header.h_sequence = handle->h_sequence + 1;
        sb->s_errno = 0;

        /* Write back */
        ret = jbd2_write_fs_block(journal, 0, sb_buf);
        kfree(sb_buf);

        if (ret != JBD2_OK) {
            kprintf("[jbd2]  failed to write updated superblock\n");
            goto out_err;
        }

        kprintf("[jbd2]  superblock updated: start=%u, seq=%u\n",
                start_pos, handle->h_sequence + 1);
    }

    /* Free handle resources */
    for (i = 0; i < handle->h_num_blocks; i++) {
        if (handle->h_data[i])
            kfree(handle->h_data[i]);
    }
    kfree(handle->h_fs_blocknrs);
    kfree(handle->h_data);
    if (handle->h_revoke_blocks)
        kfree(handle->h_revoke_blocks);
    handle->h_state = JBD2_T_STATE_DONE;
    kfree(handle);

    kprintf("[jbd2]  transaction seq=%u committed (%u block(s))\n",
            handle->h_sequence, num_tags);
    return (int)num_tags;

out_err:
    /* Free handle resources on error */
    if (handle) {
        for (i = 0; i < handle->h_num_blocks; i++) {
            if (handle->h_data[i])
                kfree(handle->h_data[i]);
        }
        if (handle->h_fs_blocknrs)
            kfree(handle->h_fs_blocknrs);
        if (handle->h_data)
            kfree(handle->h_data);
        if (handle->h_revoke_blocks)
            kfree(handle->h_revoke_blocks);
        handle->h_state = JBD2_T_STATE_DONE;
        kfree(handle);
    }
    return ret;
}

/* ── Read one journal block ─────────────────────────────────────────── */
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

/* Forward declarations for revocation helpers used during replay */
static int jbd2_is_block_revoked(const struct jbd2_journal *journal,
                                  uint32_t blocknr);

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
            /* Check if this block has been revoked */
            if (jbd2_is_block_revoked(journal, target_block)) {
                kprintf("[jbd2]    skipping revoked block %u\n",
                        target_block);
                goto skip_block;
            }

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

skip_block:
        data_current++;
        tag_offset += sizeof(struct jbd2_block_tag);
    }

    /* Advance past all data blocks */
    *current = data_current;
    return blocks_replayed;
}

/* ── Helper: check if a block is in the revocation list ───────────── */

/*
 * jbd2_is_block_revoked — check if a block number is in the revoked list.
 *
 * @journal:   initialized journal structure (with revoke_list populated)
 * @blocknr:   filesystem block number to check
 *
 * Returns: 1 if revoked, 0 if not.
 */
static int jbd2_is_block_revoked(const struct jbd2_journal *journal,
                                  uint32_t blocknr)
{
    uint32_t i;

    if (!journal || !journal->revoke_list)
        return 0;

    for (i = 0; i < journal->revoke_count; i++) {
        if (journal->revoke_list[i] == blocknr)
            return 1;
    }
    return 0;
}

/* ── Revocation block processing during recovery ──────────────────── */

/*
 * Process a revocation block during journal recovery.
 *
 * Reads the revocation block header and records all revoked block
 * numbers in the journal's revoke_list.  During subsequent descriptor
 * replay, blocks in this list will be skipped.
 *
 * @journal:   initialized journal structure
 * @buf:       buffer containing the revocation block data
 *
 * Returns: 0 on success, negative error code on failure.
 */
static int jbd2_process_revoke_block(struct jbd2_journal *journal,
                                      const uint8_t *buf)
{
    const struct jbd2_revoke_header *r_hdr;
    uint32_t count;
    uint32_t num_records;
    const uint32_t *records;
    uint32_t i;
    uint32_t new_capacity;
    uint32_t *new_list;

    if (!journal || !buf)
        return -EINVAL;

    r_hdr = (const struct jbd2_revoke_header *)buf;

    if (r_hdr->r_header.h_magic != JBD2_MAGIC_NUMBER)
        return JBD2_ERR_BAD_MAGIC;

    /* Number of bytes used in the revoke data area */
    count = r_hdr->r_count;

    if (count == 0 || count > journal->block_size - sizeof(*r_hdr)) {
        kprintf("[jbd2]  invalid revoke count: %u\n", count);
        return JBD2_ERR_BAD_MAGIC;
    }

    num_records = count / sizeof(uint32_t);
    if (num_records == 0)
        return 0;

    records = (const uint32_t *)(buf + sizeof(*r_hdr));

    kprintf("[jbd2]  processing revoke block: %u block(s)\n",
            num_records);

    /* Ensure capacity in the journal's revoke list */
    if (journal->revoke_count + num_records > journal->revoke_capacity) {
        new_capacity = journal->revoke_capacity + num_records + 64;
        if (new_capacity < 128)
            new_capacity = 128;

        new_list = (uint32_t *)krealloc(journal->revoke_list,
            new_capacity * sizeof(uint32_t));
        if (!new_list)
            return -ENOMEM;

        journal->revoke_list = new_list;
        journal->revoke_capacity = new_capacity;
    }

    /* Append revoked block numbers to the list */
    for (i = 0; i < num_records; i++) {
        uint32_t blk = records[i];

        /* Avoid duplicates */
        if (!jbd2_is_block_revoked(journal, blk)) {
            journal->revoke_list[journal->revoke_count] = blk;
            journal->revoke_count++;
        }
    }

    kprintf("[jbd2]  revoke processed: total %u revoked block(s)\n",
            journal->revoke_count);

    return 0;
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
        /* Handle revocation blocks during recovery */
        if (hdr->h_blocktype == JBD2_REVOKE_BLOCK) {
            ret = jbd2_process_revoke_block(journal, buf);
            if (ret != JBD2_OK) {
                kprintf("[jbd2]  failed to process revoke block "
                        "at %u\n", current);
                goto out_err;
            }
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
    /* Free revocation list if allocated */
    if (journal->revoke_list) {
        kfree(journal->revoke_list);
        journal->revoke_list = NULL;
        journal->revoke_count = 0;
        journal->revoke_capacity = 0;
    }
    kprintf("[jbd2] journal recovery complete: %d transaction(s) "
            "replayed\n", transactions);
    return transactions;

out_err:
    kfree(buf);
    /* Free revocation list on error */
    if (journal->revoke_list) {
        kfree(journal->revoke_list);
        journal->revoke_list = NULL;
        journal->revoke_count = 0;
        journal->revoke_capacity = 0;
    }
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
