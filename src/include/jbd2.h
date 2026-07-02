#ifndef JBD2_H
#define JBD2_H

/*
 * src/include/jbd2.h — JBD2 (Journaling Block Device 2) data structures.
 *
 * Implements the on-disk format for the ext4 journaling layer (JBD2).
 * Uses the Linux JBD2 v2 superblock format for compatibility with
 * ext4 filesystems created by mkfs.ext4.
 *
 * Part of D177: ext4 journaling support.
 */

#include "types.h"

/* ── Magic numbers ──────────────────────────────────────────────────── */

#define JBD2_MAGIC_NUMBER    0xc03b3998

/* ── Journal block types (all blocks start with a jbd2_header_t) ────── */

#define JBD2_DESCRIPTOR_BLOCK  1  /* Transaction descriptor block */
#define JBD2_COMMIT_BLOCK      2  /* Transaction commit block */
#define JBD2_SUPERBLOCK_V1     3  /* V1 journal superblock */
#define JBD2_SUPERBLOCK_V2     4  /* V2 journal superblock (ext4 default) */
#define JBD2_REVOKE_BLOCK      5  /* Revocation block */

/* ── Journal feature flags ──────────────────────────────────────────── */

/* Compatible features (any journal can read) */
#define JBD2_FEATURE_COMPAT_CHECKSUM      0x00000001  /* Superblock has checksum */

/* Incompatible features (reader must understand) */
#define JBD2_FEATURE_INCOMPAT_REVOKE       0x00000001  /* Revoke records supported */
#define JBD2_FEATURE_INCOMPAT_64BIT        0x00000002  /* 64-bit block numbers */
#define JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT 0x00000004  /* Async commit (no commit block wait) */
#define JBD2_FEATURE_INCOMPAT_CSUM_V2      0x00000008  /* V2 checksum (CRC32C) */
#define JBD2_FEATURE_INCOMPAT_CSUM_V3      0x00000010  /* V3 checksum (UUID + CRC32C) */

/* Read-only compatible features */
#define JBD2_FEATURE_RO_COMPAT_UNUSED       0x00000001

/* ── Descriptor block tag flags ─────────────────────────────────────── */

#define JBD2_FLAG_ESCAPE        0x00000001  /* Block was replaced with JBD2 magic */
#define JBD2_FLAG_SAME_UUID     0x00000002  /* Block has same UUID as journal */
#define JBD2_FLAG_DELETED       0x00000004  /* Block deleted by revoke */
#define JBD2_FLAG_LAST_TAG      0x00000008  /* Last tag in descriptor block */

/* ── Size limits ────────────────────────────────────────────────────── */

#define JBD2_MIN_BLOCKS         64
#define JBD2_DEFAULT_MAXLEN     32768
#define JBD2_MAX_BLOCK_SIZE     4096
#define JBD2_SUPERBLOCK_SIZE    1024

/* ── On-disk structures (all big-endian on disk, stored here in CPU order) ── */

/*
 * Standard journal block header — every journal block begins with this.
 * On-disk format uses big-endian; loaded fields are converted to CPU order.
 */
struct jbd2_header {
    uint32_t h_magic;       /* Must be JBD2_MAGIC_NUMBER */
    uint32_t h_blocktype;   /* One of JBD2_*_BLOCK */
    uint32_t h_sequence;    /* Transaction sequence number */
} __attribute__((packed));

/*
 * Journal superblock V2 — stored at journal block 0.
 *
 * Layout (all fields big-endian on disk):
 *   Offset  Size  Field
 *   0       12    s_header (magic + blocktype + sequence)
 *   12      4     s_blocksize
 *   16      4     s_maxlen
 *   20      4     s_first
 *   24      4     s_sequence
 *   28      4     s_start
 *   32      4     s_errno
 *   ── V2 fields ──
 *   36      4     s_feature_compat
 *   40      4     s_feature_incompat
 *   44      4     s_feature_ro_compat
 *   48      16    s_uuid
 *   64      4     s_nr_users
 *   68      4     s_dynsuper
 *   72      4     s_max_transaction
 *   76      4     s_max_trans_data
 *   80      1     s_checksum_type
 *   81      3     s_padding2
 *   84      168   s_padding[42]
 *   252     4     s_checksum
 */
struct jbd2_superblock {
    /* Common header */
    struct jbd2_header s_header;        /* 0-11: magic, blocktype, sequence */

    /* V1 journal geometry */
    uint32_t s_blocksize;               /* 12-15: Journal block size in bytes */
    uint32_t s_maxlen;                  /* 16-19: Total blocks in journal */
    uint32_t s_first;                   /* 20-23: First block of transaction data */
    uint32_t s_sequence;                /* 24-27: First transaction ID expected */
    uint32_t s_start;                   /* 28-31: Block offset of log start */
    uint32_t s_errno;                   /* 32-35: Error from last replay */

    /* V2 fields */
    uint32_t s_feature_compat;          /* 36-39 */
    uint32_t s_feature_incompat;        /* 40-43 */
    uint32_t s_feature_ro_compat;       /* 44-47 */
    uint8_t  s_uuid[16];                /* 48-63: Journal UUID */
    uint32_t s_nr_users;                /* 64-67: Number of filesystems using this journal */
    uint32_t s_dynsuper;                /* 68-71: Dynamic superblock block number */
    uint32_t s_max_transaction;         /* 72-75: Max blocks per transaction */
    uint32_t s_max_trans_data;          /* 76-79: Max data blocks per transaction */
    uint8_t  s_checksum_type;           /* 80: Checksum algorithm (0=CRC32, 1=CRC32C, etc.) */
    uint8_t  s_padding2[3];             /* 81-83 */
    uint32_t s_padding[42];             /* 84-251: Reserved for future use */
    uint32_t s_checksum;                /* 252-255: Superblock CRC32 */
    /* Total: 256 bytes (rest of block is padding) */
} __attribute__((packed));

/* Descriptor block tag (V1, 8 bytes per tag) */
struct jbd2_block_tag {
    uint32_t t_blocknr;     /* Block number to replay */
    uint32_t t_flags;       /* JBD2_FLAG_* flags */
} __attribute__((packed));

/* Descriptor block tag (V2, 12 bytes with high 32 bits of block number) */
struct jbd2_block_tag_v2 {
    uint32_t t_blocknr;          /* Lower 32 bits of block number */
    uint32_t t_flags;            /* JBD2_FLAG_* flags */
    uint32_t t_blocknr_high;     /* Upper 32 bits of block number (for 64-bit mode) */
} __attribute__((packed));

/* Revocation block header */
struct jbd2_revoke_header {
    struct jbd2_header r_header;        /* Standard journal header (12 bytes) */
    uint32_t r_count;                   /* Number of bytes used in revoke data area */
    uint32_t r_padding[5];              /* Padding to fill rest of first block part */
} __attribute__((packed));

/* ── In-memory journal state ────────────────────────────────────────── */

/* Journal state flags */
#define JBD2_STATE_CLEAN      0  /* Journal is clean (no recovery needed) */
#define JBD2_STATE_DIRTY      1  /* Journal has uncommitted transactions */
#define JBD2_STATE_ERROR      2  /* Journal encountered an error */

/* In-memory representation of a loaded journal */
struct jbd2_journal {
    uint8_t  dev_id;             /* Block device ID hosting the journal */
    uint32_t inum;               /* Journal inode number (0 = external device) */
    uint32_t block_size;         /* Journal block size (bytes) */
    uint32_t total_blocks;       /* Total blocks in journal (s_maxlen) */
    uint32_t first_data_block;   /* First block of transaction data (s_first) */
    uint32_t sequence;           /* Next expected transaction ID (s_sequence) */
    uint32_t start_block;        /* Start of active transaction log (s_start) */
    uint32_t errno_val;          /* Journal-level error code (s_errno) */
    uint32_t compat;             /* Compat feature flags */
    uint32_t incompat;           /* Incompat feature flags */
    uint32_t ro_compat;          /* Read-only compat feature flags */
    uint8_t  uuid[16];           /* Journal UUID */

    /* Revocation tracking for recovery */
    uint32_t *revoke_list;       /* Array of revoked block numbers */
    uint32_t  revoke_count;      /* Number of entries in revoke_list */
    uint32_t  revoke_capacity;   /* Allocated capacity of revoke_list */
};

/* ── Transaction commit ─────────────────────────────────────────────── */

/* Transaction handle state */
#define JBD2_T_STATE_ACTIVE     0
#define JBD2_T_STATE_COMMITTING 1
#define JBD2_T_STATE_DONE       2

/* Maximum tags that fit in one descriptor block for 4096-byte blocks.
 * Actual max depends on block_size; this is the upper bound. */
#define JBD2_MAX_TAGS_PER_BLOCK \
    ((4096 - (int)sizeof(struct jbd2_header)) / (int)sizeof(struct jbd2_block_tag))

/* Maximum revocation blocks that can fit in one journal block (V1, 32-bit) */
#define JBD2_MAX_REVOKES_PER_BLOCK \
    (((int)sizeof(struct jbd2_block_tag) <= 0) ? 0 : \
     ((4096 - (int)sizeof(struct jbd2_revoke_header)) / (int)sizeof(uint32_t)))

/* In-memory transaction handle.
 *
 * Created by jbd2_journal_start(), populated by jbd2_journal_get_write_access()
 * and jbd2_journal_dirty_metadata(), and finalized by jbd2_commit_transaction()
 * or jbd2_journal_stop().
 *
 * The handle tracks one atomic journal transaction.  During commit, a
 * descriptor block, all data blocks, and a commit block are written to the
 * journal device, atomically finalizing the transaction.  Optionally,
 * revocation blocks are written before the commit block to record blocks
 * that should not be replayed during recovery.
 */
struct jbd2_handle {
    struct jbd2_journal *h_journal;   /* Owning journal */
    uint32_t h_sequence;              /* Transaction sequence number */
    uint32_t h_num_blocks;            /* Number of blocks currently registered */
    uint32_t h_capacity;              /* Max blocks this handle can hold */
    int      h_state;                 /* JBD2_T_STATE_* */

    /* Arrays of registered blocks (parallel arrays, length = h_capacity) */
    uint32_t  *h_fs_blocknrs;         /* Filesystem block numbers */
    uint8_t  **h_data;                /* Data block content to journal */

    /* Revocation tracking */
    uint32_t *h_revoke_blocks;       /* Blocks to revoke in this transaction */
    uint32_t  h_revoke_count;        /* Number of revoked blocks */
    uint32_t  h_revoke_capacity;     /* Allocated capacity of revoke array */
};

/* ── Return values / error codes ────────────────────────────────────── */

#define JBD2_OK              0  /* Success */
#define JBD2_ERR_BAD_MAGIC  -1  /* Invalid magic number or block type */
#define JBD2_ERR_BLOCK_SIZE -2  /* Invalid or mismatched block size */
#define JBD2_ERR_IO         -3  /* Block device I/O error */
#define JBD2_ERR_CHECKSUM   -4  /* Superblock checksum mismatch */

/* ── Journal replay ────────────────────────────────────────────────── */

/*
 * jbd2_replay — replay committed transactions from the journal.
 *
 * @journal: initialized journal structure (loaded via jbd2_load_superblock)
 *
 * Scans the journal from s_start, reads each committed transaction,
 * replays data blocks to their target filesystem locations, and
 * marks the journal clean upon successful completion.
 *
 * Returns: number of transactions replayed on success (0 = nothing to do),
 *          negative error code (< 0) on failure.
 */
int jbd2_replay(struct jbd2_journal *journal);

/* ── Public API ─────────────────────────────────────────────────────── */

/*
 * jbd2_check_superblock — validate a loaded JBD2 superblock.
 *
 * @sb:         pointer to the loaded superblock
 * @block_size: expected block size (0 = accept any valid size)
 *
 * Returns: JBD2_OK on success, negative error code on failure.
 *
 * Validates magic, block type, block size, and geometry sanity.
 */
int jbd2_check_superblock(const struct jbd2_superblock *sb,
                           uint32_t block_size);

/*
 * jbd2_load_superblock — read and parse the JBD2 superblock from a device.
 *
 * @journal:     [out] populated journal structure on success
 * @dev_id:      block device ID
 * @journal_inum: journal inode number (0 = external device, superblock at block 0)
 * @block_size:  expected filesystem block size for validation (0 = accept any)
 *
 * For external journal devices, the superblock is read from block 0.
 * For inode-based journals, block 0 of the journal inode is read.
 *
 * Returns: JBD2_OK on success, negative error code on failure.
 */
int jbd2_load_superblock(struct jbd2_journal *journal, uint8_t dev_id,
                          uint32_t journal_inum, uint32_t block_size);

/*
 * jbd2_get_state — determine journal state from the superblock.
 *
 * @journal: initialized journal structure
 *
 * Returns: JBD2_STATE_CLEAN if the journal is empty/clean,
 *          JBD2_STATE_DIRTY if transactions need replay,
 *          JBD2_STATE_ERROR if journal error is recorded.
 */
int jbd2_get_state(const struct jbd2_journal *journal);

/* ── Transaction commit API ─────────────────────────────────────────── */

/*
 * jbd2_journal_start — begin a new journal transaction.
 *
 * @journal:   initialized journal structure
 * @max_blocks: maximum number of blocks to be journaled in this transaction
 *
 * Allocates and returns a new transaction handle.  The caller must later
 * call either jbd2_commit_transaction() to commit, or jbd2_journal_stop()
 * to discard.
 *
 * Returns: pointer to handle on success, NULL on failure (allocation error).
 */
struct jbd2_handle *jbd2_journal_start(struct jbd2_journal *journal,
                                        uint32_t max_blocks);

/*
 * jbd2_journal_get_write_access — register a block for journaling.
 *
 * @handle:      active transaction handle
 * @fs_blocknr:  filesystem block number of the block being journaled
 * @data:        pointer to the block's data content (will be copied)
 *
 * The block's data is copied to an internal buffer so it can be written
 * to the journal during commit.  The caller may overwrite the original
 * block after this call returns.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int jbd2_journal_get_write_access(struct jbd2_handle *handle,
                                   uint32_t fs_blocknr,
                                   const uint8_t *data);

/*
 * jbd2_journal_dirty_metadata — mark a previously-registered block as dirty.
 *
 * @handle:      active transaction handle
 * @fs_blocknr:  filesystem block number (must match a previously registered block)
 * @data:        updated data content (will replace the saved copy)
 *
 * In this implementation, this is identical to get_write_access except
 * that it updates the saved data for an already-registered block.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int jbd2_journal_dirty_metadata(struct jbd2_handle *handle,
                                 uint32_t fs_blocknr,
                                 const uint8_t *data);

/*
 * jbd2_commit_transaction — commit the transaction to the journal.
 *
 * @handle: active transaction handle (freed by this call)
 *
 * Writes a descriptor block, all registered data blocks, and a commit
 * block to the journal device.  On success the journal superblock is
 * updated to reflect the new transaction.
 *
 * Returns: number of blocks committed on success (> 0),
 *          negative errno on failure.
 */
int jbd2_commit_transaction(struct jbd2_handle *handle);

/*
 * jbd2_journal_stop — discard an uncommitted transaction handle.
 *
 * @handle: transaction handle to discard (freed by this call)
 *
 * Releases all resources associated with the handle without writing
 * anything to the journal.
 */
void jbd2_journal_stop(struct jbd2_handle *handle);

/*
 * jbd2_journal_revoke — revoke a block in the current transaction.
 *
 * @handle:   active transaction handle
 * @fs_blocknr: filesystem block number to revoke
 *
 * Revoked blocks are recorded in a revocation block written before the
 * commit block.  During journal recovery, revoked blocks are NOT replayed,
 * even if they appear in a descriptor block.  This is used when a block
 * was allocated and then freed within the same transaction — replaying
 * the old data would corrupt the filesystem.
 *
 * Returns: 0 on success, negative errno on failure.
 */
int jbd2_journal_revoke(struct jbd2_handle *handle, uint32_t fs_blocknr);

/*
 * jbd2_init — initialize the JBD2 subsystem.
 *
 * Called at boot via device_initcall.
 */
int jbd2_init(void);

#endif /* JBD2_H */
