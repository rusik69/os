#ifndef EXT4_EXTENTS_H
#define EXT4_EXTENTS_H

/*
 * src/include/ext4_extents.h — Ext4 extent tree public API.
 *
 * Provides extent tree traversal and block resolution for ext4
 * filesystems.  Used by ext4.c and companion filesystem modules.
 */

#include "types.h"
#include "ext4.h"

/* Extent header magic */
#define EXT4_EXTENT_MAGIC      0xF30A

/* Maximum depth of extent tree (sanity bound) */
#define EXT4_EXTENT_MAX_DEPTH  5

/* Maximum blocks covered by a single extent */
#define EXT4_EXT_MAX_LEN       (1UL << 15)

/* ── Extent tree traversal ─────────────────────────────────────────── */

/*
 * ext4_ext_find_extent — resolve a logical block to a physical block
 *                        via the extent tree.
 *
 * @ep:     ext4 private per-mount data
 * @inode:  inode with EXT4_EXTENTS_FL set
 * @iblock: logical block number to resolve
 *
 * Returns:
 *   >0  physical block number
 *    0  hole (sparse or uninitialized extent)
 *   -1  fatal error (filesystem corrupted)
 */
int64_t ext4_ext_find_extent(struct ext4_priv *ep,
                             struct ext4_inode *inode,
                             uint32_t iblock);

/*
 * ext4_ext_get_blocks — resolve logical block(s) and report extent info.
 *
 * @ep:         ext4 private per-mount data
 * @inode:      inode with EXT4_EXTENTS_FL set
 * @iblock:     logical block number to resolve
 * @max_blocks: [in/out] in: max blocks caller wants; out: contiguous blocks
 * @phys_block: [out] physical block number (0 for hole)
 * @uninit:     [out] non-zero if this is an uninitialized extent
 *
 * Returns 0 on success, negative errno on failure.
 */
int ext4_ext_get_blocks(struct ext4_priv *ep,
                        struct ext4_inode *inode,
                        uint32_t iblock,
                        uint32_t *max_blocks,
                        uint64_t *phys_block,
                        int *uninit);

/* ── Validation helpers ────────────────────────────────────────────── */

/*
 * ext4_ext_check_header — validate an extent tree header.
 *
 * Validates magic number, depth bounds, and entry counts.
 * Returns 0 if valid, -EFSCORRUPTED if invalid.
 */
int ext4_ext_check_header(struct ext4_extent_header *eh, uint16_t max_depth);

/* ── Extent tree manipulation ──────────────────────────────────────── */

/*
 * ext4_ext_insert_extent — insert a new extent into the extent tree.
 *
 * @ep:     ext4 private per-mount data
 * @inode:  inode to modify (must have EXT4_EXTENTS_FL set)
 * @newext: the new extent to insert
 *
 * Handles merging with adjacent extents, leaf node insertion, and
 * node splitting when the target leaf is full.
 *
 * Returns 0 on success, negative errno on failure.
 */
int ext4_ext_insert_extent(struct ext4_priv *ep,
                           struct ext4_inode *inode,
                           struct ext4_extent *newext);

/*
 * ext4_ext_remove_space — remove blocks in [start, end) from the
 *                         extent tree.
 *
 * @ep:     ext4 private per-mount data
 * @inode:  inode to modify
 * @start:  first logical block to remove
 * @end:    first logical block NOT to remove
 *
 * Handles truncation, hole-punching.  Splits partially-overlapping
 * extents and removes fully-covered extents.
 *
 * Returns 0 on success, negative errno on failure.
 */
int ext4_ext_remove_space(struct ext4_priv *ep,
                          struct ext4_inode *inode,
                          uint32_t start,
                          uint32_t end);

/* ── Journaling for extent metadata ────────────────────────────────── */

/*
 * ext4_ext_journal_init — associate a JBD2 journal with ext4 extents.
 *
 * @ep:      ext4 private per-mount data
 * @journal: initialized JBD2 journal structure
 *
 * Stores the journal pointer in ext4_priv for use by all extent
 * manipulation functions.  After calling this, subsequent extent
 * insert/remove operations will journal their metadata block writes.
 */
void ext4_ext_journal_init(struct ext4_priv *ep,
                           struct jbd2_journal *journal);

/*
 * ext4_ext_journal_start — begin a JBD2 transaction for extent metadata.
 *
 * @ep:          ext4 private per-mount data
 * @max_blocks:  maximum number of metadata blocks to be modified
 *
 * Stores the transaction handle in ep->journal_handle.
 * Returns 0 on success, negative errno on failure.
 * If ep->journal is NULL, returns 0 (no-op, direct-write mode).
 */
int ext4_ext_journal_start(struct ext4_priv *ep, uint32_t max_blocks);

/*
 * ext4_ext_journal_commit — commit the current extent metadata transaction.
 *
 * @ep: ext4 private per-mount data
 *
 * Commits and frees the handle, clears ep->journal_handle.
 * Returns number of blocks committed on success (> 0),
 *         0 if no journal or no active transaction,
 *         negative errno on failure.
 */
int ext4_ext_journal_commit(struct ext4_priv *ep);

/*
 * ext4_ext_journal_stop — discard the current transaction without commit.
 *
 * @ep: ext4 private per-mount data
 *
 * Frees the handle without writing to the journal.
 */
void ext4_ext_journal_stop(struct ext4_priv *ep);

/*
 * ext4_ext_journal_get_write_access — register a block with the journal
 *                                     before modification.
 *
 * @ep:         ext4 private per-mount data
 * @block_num:  filesystem block number to register
 * @data:       current block content (will be copied for journal)
 *
 * Must be called BEFORE modifying a metadata block.  Returns 0 on
 * success or if no journal is present, negative errno on failure.
 */
int ext4_ext_journal_get_write_access(struct ext4_priv *ep,
                                      uint32_t block_num,
                                      const uint8_t *data);

/*
 * ext4_ext_journal_dirty_block — write a modified metadata block and
 *                                mark it dirty in the journal.
 *
 * @ep:         ext4 private per-mount data
 * @block_num:  filesystem block number that was modified
 * @data:       modified block content
 *
 * Writes the block to its on-disk location and, if a transaction is
 * active, logs it as dirty metadata so JBD2 records it atomically.
 * Returns 0 on success, negative errno on failure.
 */
int ext4_ext_journal_dirty_block(struct ext4_priv *ep,
                                 uint32_t block_num,
                                 const uint8_t *data);

/* ── Module init ───────────────────────────────────────────────────── */

int ext4_ext_init(void);

#endif /* EXT4_EXTENTS_H */
