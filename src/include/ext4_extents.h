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

/* ── Module init ───────────────────────────────────────────────────── */

int ext4_ext_init(void);

#endif /* EXT4_EXTENTS_H */
