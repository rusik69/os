/*
 * src/fs/ext4_extents.c — Ext4 extent tree traversal and management.
 *
 * Implements the extent tree walk used by ext4 to resolve logical block
 * numbers to physical block numbers on disk.  Supports multi-level trees
 * up to EXT4_EXTENT_MAX_DEPTH with binary search at each internal node.
 *
 * Part of the Hermes OS ext4 filesystem implementation.
 *
 * Task 4: extent tree manipulation — split, merge, insert, remove.
 */

#define KERNEL_INTERNAL
#include "ext4_extents.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "initcall.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Forward declarations of internal helpers ──────────────────────── */

static uint16_t ext4_ext_max_entries(uint16_t depth, uint32_t block_size);
static int      ext4_ext_binsearch_idx(struct ext4_extent_header *eh,
                                       uint32_t iblock,
                                       struct ext4_extent_idx **ret_idx);
static int      ext4_ext_binsearch(struct ext4_extent_header *eh,
                                   uint32_t iblock,
                                   struct ext4_extent **ret_ext);
static int      ext4_ext_write_block(struct ext4_priv *ep, uint32_t block_num,
                                     const uint8_t *buf);
static int      ext4_ext_merge(struct ext4_extent_header *eh);
static int      ext4_ext_insert_in_a_block(struct ext4_extent_header *eh,
                                           struct ext4_extent *newext,
                                           int pos);
static int      ext4_ext_split(struct ext4_priv *ep,
                               struct ext4_extent_header *old_eh,
                               uint32_t old_block,
                               struct ext4_extent_header **new_eh,
                               uint32_t *new_block,
                               int depth);
static int      ext4_ext_find_insert_position(struct ext4_extent_header *eh,
                                              struct ext4_extent *newext,
                                              int *pos);

/* ── JBD2 journal helpers for extent metadata journaling ──────────── */

/*
 * ext4_ext_journal_init — associate a JBD2 journal with ext4 extents.
 */
void ext4_ext_journal_init(struct ext4_priv *ep,
                           struct jbd2_journal *journal)
{
    if (!ep)
        return;
    ep->journal = journal;
    ep->journal_handle = NULL;
    kprintf("[ext4_ext] journal initialized for extent metadata journaling\n");
}

/*
 * ext4_ext_journal_start — begin a JBD2 transaction for extent metadata.
 */
int ext4_ext_journal_start(struct ext4_priv *ep, uint32_t max_blocks)
{
    if (!ep)
        return -EINVAL;

    /* No journal — direct-write mode, no-op */
    if (!ep->journal)
        return 0;

    ep->journal_handle = jbd2_journal_start(ep->journal, max_blocks);
    if (!ep->journal_handle)
        return -ENOMEM;

    return 0;
}

/*
 * ext4_ext_journal_commit — commit the current extent metadata transaction.
 */
int ext4_ext_journal_commit(struct ext4_priv *ep)
{
    struct jbd2_handle *handle;
    int ret;

    if (!ep || !ep->journal_handle)
        return 0;

    handle = ep->journal_handle;
    ep->journal_handle = NULL;

    ret = jbd2_commit_transaction(handle);
    return ret;
}

/*
 * ext4_ext_journal_stop — discard the current transaction without commit.
 */
void ext4_ext_journal_stop(struct ext4_priv *ep)
{
    struct jbd2_handle *handle;

    if (!ep || !ep->journal_handle)
        return;

    handle = ep->journal_handle;
    ep->journal_handle = NULL;
    jbd2_journal_stop(handle);
}

/*
 * ext4_ext_journal_get_write_access — register a block with the journal
 *                                     BEFORE modification.
 */
int ext4_ext_journal_get_write_access(struct ext4_priv *ep,
                                      uint32_t block_num,
                                      const uint8_t *data)
{
    if (!ep || !ep->journal_handle)
        return 0;

    return jbd2_journal_get_write_access(ep->journal_handle,
                                          block_num, data);
}

/*
 * ext4_ext_journal_dirty_block — write a modified metadata block and
 *                                mark it dirty in the journal.
 */
int ext4_ext_journal_dirty_block(struct ext4_priv *ep,
                                 uint32_t block_num,
                                 const uint8_t *data)
{
    int ret;

    if (!ep)
        return -EINVAL;

    /* Always write the block to its on-disk location first */
    ret = ext4_ext_write_block(ep, block_num, data);
    if (ret < 0)
        return ret;

    /* If a JBD2 transaction is active, mark the block as dirty metadata */
    if (ep->journal_handle) {
        ret = jbd2_journal_dirty_metadata(ep->journal_handle,
                                           block_num, data);
        if (ret < 0)
            return ret;
    }

    return 0;
}

/* ── Validation ────────────────────────────────────────────────────── */

int ext4_ext_check_header(struct ext4_extent_header *eh, uint16_t max_depth)
{
    if (!eh)
        return -EFSCORRUPTED;

    if (eh->eh_magic != EXT4_EXTENT_MAGIC) {
        kprintf("[ext4_ext] bad extent magic: 0x%04x (expected 0x%04x)\n",
                eh->eh_magic, EXT4_EXTENT_MAGIC);
        return -EFSCORRUPTED;
    }

    if (eh->eh_depth > max_depth) {
        kprintf("[ext4_ext] extent tree depth %u exceeds max %u\n",
                eh->eh_depth, max_depth);
        return -EFSCORRUPTED;
    }

    if (eh->eh_entries > eh->eh_max) {
        kprintf("[ext4_ext] entries %u exceeds capacity %u\n",
                eh->eh_entries, eh->eh_max);
        return -EFSCORRUPTED;
    }

    return 0;
}

/* ── Capacity calculation ──────────────────────────────────────────── */

/*
 * Return the maximum number of entries (extent or index) that can fit
 * in a block at the given depth.  At depth 0 (leaf), entries are
 * struct ext4_extent (12 bytes).  At depth > 0 (index), entries are
 * struct ext4_extent_idx (12 bytes).  In both cases the header is
 * sizeof(struct ext4_extent_header) = 12 bytes.
 */
static uint16_t ext4_ext_max_entries(uint16_t depth, uint32_t block_size)
{
    /* Leaf extents (ext4_extent) and index entries (ext4_extent_idx)
     * are both 12 bytes packed — verified by build-time assertion. */
    _Static_assert(sizeof(struct ext4_extent) == sizeof(struct ext4_extent_idx),
                   "extent leaf and index must be same size (12 bytes)");
    uint32_t entry_size = sizeof(struct ext4_extent);

    uint32_t avail = (uint32_t)(block_size - sizeof(struct ext4_extent_header));
    uint16_t max = (uint16_t)(avail / entry_size);

    if (max > 0x7FFF)
        max = 0x7FFF;

    return max;
}

/* ── Binary search helpers ─────────────────────────────────────────── */

/*
 * Binary search in an index (internal) node.  Finds the index entry
 * whose ei_block is the greatest <= iblock.  Returns 0 on success
 * (found >= 0).  Returns -1 if iblock is before the first entry
 * (hole at start).
 */
static int ext4_ext_binsearch_idx(struct ext4_extent_header *eh,
                                  uint32_t iblock,
                                  struct ext4_extent_idx **ret_idx)
{
    struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
    uint16_t num_entries = eh->eh_entries;

    if (num_entries == 0)
        return -1;

    int lo = 0, hi = (int)num_entries - 1, found = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (iblock >= idx[mid].ei_block) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (found < 0)
        return -1;

    if (ret_idx)
        *ret_idx = &idx[found];

    return 0;
}

/*
 * Binary search in a leaf (extent) node.  Finds the extent that
 * covers iblock.  Returns 0 if found, -1 if not found (hole).
 */
static int ext4_ext_binsearch(struct ext4_extent_header *eh,
                              uint32_t iblock,
                              struct ext4_extent **ret_ext)
{
    struct ext4_extent *ext = (struct ext4_extent *)(eh + 1);
    uint16_t num_entries = eh->eh_entries;

    if (num_entries == 0)
        return -1;

    int lo = 0, hi = (int)num_entries - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (iblock < ext[mid].ee_block) {
            hi = mid - 1;
        } else {
            uint16_t len = ext[mid].ee_len & ~0x8000;
            if (len == 0)
                len = 32768;
            /* Use subtraction to avoid overflow: ee_block + len can wrap
             * when ee_block is near UINT32_MAX, causing incorrect extent
             * lookup.  Since iblock >= ee_block at this point, the
             * subtraction (iblock - ee_block) is well-defined. */
            if ((iblock - ext[mid].ee_block) < len) {
                if (ret_ext)
                    *ret_ext = &ext[mid];
                return 0;
            }
            lo = mid + 1;
        }
    }

    return -1; /* not found — hole */
}

/* ── Block I/O helpers ─────────────────────────────────────────────── */

/*
 * Write a single filesystem block (ep->block_size bytes) to disk.
 * Used to write back modified extent tree nodes.
 * If a JBD2 transaction is active (ep->journal_handle != NULL), the
 * block is also registered as dirty metadata for journaling.
 */
static int ext4_ext_write_block(struct ext4_priv *ep, uint32_t block_num,
                                const uint8_t *buf)
{
    uint64_t lba = (uint64_t)block_num * (ep->block_size / 512);
    uint32_t sectors = ep->block_size / 512;
    int ret;

    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_write_sectors(ep->dev_id, (uint32_t)(lba + i), 1,
                                   buf + i * 512) != 0)
            return -EIO;
    }

    /* If a JBD2 transaction is active, mark the block as dirty metadata */
    if (ep->journal_handle) {
        ret = jbd2_journal_dirty_metadata(ep->journal_handle,
                                           block_num, buf);
        if (ret < 0)
            return ret;
    }

    return 0;
}

/* ── Find the sorted insertion position for a new extent ───────────── */

/*
 * Find the position (index) in the leaf extent array where @newext
 * should be inserted.  The position is determined by sorting on
 * ee_block.  Returns 0 on success.  Returns -1 if @newext overlaps
 * with an existing extent (caller must handle this case).
 *
 * @pos: [out] insertion index (0..eh_entries)
 */
static int ext4_ext_find_insert_position(struct ext4_extent_header *eh,
                                         struct ext4_extent *newext,
                                         int *pos)
{
    struct ext4_extent *exts = (struct ext4_extent *)(eh + 1);
    uint16_t num_entries = eh->eh_entries;
    int i;

    *pos = 0;

    for (i = 0; i < (int)num_entries; i++) {
        uint32_t ext_start = exts[i].ee_block;
        uint16_t ext_len = exts[i].ee_len & ~0x8000;
        if (ext_len == 0)
            ext_len = 32768;
        /* Avoid overflow: use safe comparison instead of ext_start + ext_len */
        uint32_t ext_end = (ext_len > 0xFFFFFFFFU - ext_start)
                           ? 0xFFFFFFFFU
                           : ext_start + ext_len;

        /* Check for overlap: new extent starts before existing ends,
         * and existing starts before new ends */
        uint32_t new_len_val = newext->ee_len & ~0x8000;
        if (new_len_val == 0)
            new_len_val = 32768;
        uint32_t new_end = (new_len_val > 0xFFFFFFFFU - newext->ee_block)
                           ? 0xFFFFFFFFU
                           : newext->ee_block + new_len_val;

        if (newext->ee_block < ext_end &&
            exts[i].ee_block < new_end) {
            /* Overlap — not allowed */
            return -1;
        }

        if (newext->ee_block < exts[i].ee_block) {
            *pos = i;
            return 0;
        }
    }

    /* Insert at the end */
    *pos = num_entries;
    return 0;
}

/* ── Merge adjacent extents ────────────────────────────────────────── */

/*
 * Merge adjacent extents in a leaf node.
 *
 * Walks the extent array left-to-right and merges any two consecutive
 * extents that are both logically and physically contiguous and whose
 * combined length does not exceed 32768.
 *
 * Returns the number of extents removed by merging (0 if no merge
 * was possible).
 */
static int ext4_ext_merge(struct ext4_extent_header *eh)
{
    struct ext4_extent *exts = (struct ext4_extent *)(eh + 1);
    uint16_t entries = eh->eh_entries;
    int merged = 0;

    if (entries < 2)
        return 0;

    int i = 0;
    while (i < (int)entries - 1) {
        struct ext4_extent *a = &exts[i];
        struct ext4_extent *b = &exts[i + 1];

        uint16_t a_len = a->ee_len & ~0x8000;
        uint16_t b_len = b->ee_len & ~0x8000;

        if (a_len == 0) a_len = 32768;
        if (b_len == 0) b_len = 32768;

        /* Check if both extents are uninitialized */
        int a_uninit = !!(a->ee_len & 0x8000);
        int b_uninit = !!(b->ee_len & 0x8000);

        /* Can only merge if both same type (both init or both uninit) */
        if (a_uninit != b_uninit) {
            i++;
            continue;
        }

        /* Check logical contiguity: a ends exactly where b starts.
         * Use safe arithmetic to avoid overflow when a->ee_block + a_len
         * wraps past 0xFFFFFFFF. */
        uint32_t a_end;
        if (a_len > 0xFFFFFFFFU - a->ee_block)
            a_end = 0xFFFFFFFFU;
        else
            a_end = a->ee_block + a_len;
        if (a_end != b->ee_block) {
            i++;
            continue;
        }

        /* Check physical contiguity */
        uint64_t a_start = ((uint64_t)a->ee_start_hi << 32) | a->ee_start_lo;
        uint64_t b_start = ((uint64_t)b->ee_start_hi << 32) | b->ee_start_lo;

        if (a_start + a_len != b_start) {
            i++;
            continue;
        }

        /* Check combined length doesn't exceed max */
        uint32_t combined = a_len + b_len;
        if (combined > EXT4_EXT_MAX_LEN) {
            i++;
            continue;
        }

        /* Merge b into a */
        if (a_uninit)
            a->ee_len = (uint16_t)(combined | 0x8000);
        else
            a->ee_len = (uint16_t)combined;

        /* Shift remaining entries left */
        int remaining = entries - i - 2;
        if (remaining > 0) {
            memmove(&exts[i + 1], &exts[i + 2],
                    remaining * sizeof(struct ext4_extent));
        }

        entries--;
        eh->eh_entries = entries;
        merged++;
        /* Don't increment i — check if a can merge with the next */
    }

    return merged;
}

/* ── Insert extent into a leaf block ───────────────────────────────── */

/*
 * Insert a single extent entry into a leaf node at position @pos.
 * Shifts all entries at and after @pos right by one.
 *
 * Returns 0 on success, -ENOSPC if the leaf is full.
 */
static int ext4_ext_insert_in_a_block(struct ext4_extent_header *eh,
                                      struct ext4_extent *newext,
                                      int pos)
{
    struct ext4_extent *exts = (struct ext4_extent *)(eh + 1);
    uint16_t entries = eh->eh_entries;

    if (entries >= eh->eh_max)
        return -ENOSPC;

    if (pos < 0 || pos > (int)entries)
        return -EINVAL;

    /* Shift entries at pos right by one */
    if (pos < (int)entries) {
        memmove(&exts[pos + 1], &exts[pos],
                (entries - pos) * sizeof(struct ext4_extent));
    }

    /* Insert the new extent */
    memcpy(&exts[pos], newext, sizeof(struct ext4_extent));
    eh->eh_entries = entries + 1;

    return 0;
}

/* ── Split a full leaf or index node ───────────────────────────────── */

/*
 * Split a full extent tree node (leaf or index) by allocating a new
 * block and moving half the entries there.
 *
 * @old_eh:    header of the full node (to be split)
 * @old_block: physical block number of the node being split
 * @new_eh:    [out] header of the new (right) node
 * @new_block: [out] physical block number of the new node
 * @depth:     0 for leaf, >0 for index (determines entry type)
 *
 * Returns 0 on success, negative errno on error.
 *
 * After a successful split:
 *   - The old node retains the first half of entries.
 *   - The new node gets the second half.
 *   - The new node is written to disk.
 *   - The caller must update the parent index entry (or grow the tree).
 *
 * NOTE: Block allocation is stubbed — currently uses a very simple
 * allocation strategy.  In production this should go through the ext4
 * block allocator (ext4_alloc_block).
 */
static int ext4_ext_split(struct ext4_priv *ep,
                          struct ext4_extent_header *old_eh,
                          uint32_t old_block,
                          struct ext4_extent_header **new_eh,
                          uint32_t *new_block,
                          int depth)
{
    if (!ep || !old_eh || !new_eh || !new_block)
        return -EINVAL;

    uint16_t entries = old_eh->eh_entries;
    if (entries < 2)
        return -ENOSPC; /* can't split a node with fewer than 2 entries */

    /* ── Allocate a new block ── */
    /* Simply use old_block + 1 as a naive allocator.
     * A real implementation should call ext4 block allocation. */
    uint32_t alloc_block = old_block + 1;

    /* ── Allocate a buffer for the new node ── */
    uint8_t *new_buf = (uint8_t *)kmalloc(ep->block_size);
    if (!new_buf)
        return -ENOMEM;

    memset(new_buf, 0, ep->block_size);

    /* If journaling, register the old block before modifying it */
    {
        int journal_ret = ext4_ext_journal_get_write_access(
                              ep, old_block,
                              (const uint8_t *)old_eh);
        if (journal_ret < 0) {
            kfree(new_buf);
            return journal_ret;
        }
    }

    /* ── Set up the new node header ── */
    struct ext4_extent_header *new_header;
    new_header = (struct ext4_extent_header *)new_buf;
    new_header->eh_magic = EXT4_EXTENT_MAGIC;
    new_header->eh_depth = old_eh->eh_depth;
    new_header->eh_generation = old_eh->eh_generation;
    new_header->eh_max = ext4_ext_max_entries((uint16_t)depth, ep->block_size);

    /* ── Split entries: move the second half to the new node ── */
    uint16_t split_point = entries / 2;
    uint16_t right_count = entries - split_point;

    /* Leaf extents and index entries are same size (12 bytes packed) */
    size_t entry_size = sizeof(struct ext4_extent);
    (void)sizeof(struct ext4_extent_idx); /* prevented from diverging by _Static_assert above */

    /* Copy right half to new node */
    uint8_t *old_entries = (uint8_t *)(old_eh + 1);
    uint8_t *new_entries = (uint8_t *)(new_header + 1);

    memcpy(new_entries, old_entries + split_point * entry_size,
           right_count * entry_size);
    new_header->eh_entries = right_count;

    /* Shrink old node to left half only */
    old_eh->eh_entries = split_point;

    /* ── Update the first index entry in the new node if it's an index
     *     node (index entries need their ei_block updated to reflect
     *     the first block of the subtree).  For leaf nodes the
     *     extents already have correct ee_block. ── */

    /* ── Write the new node to disk ── */
    int ret = ext4_ext_write_block(ep, alloc_block, new_buf);
    if (ret < 0) {
        kfree(new_buf);
        return ret;
    }

    /* Write the old node back (it was modified — entries count changed) */
    ret = ext4_ext_write_block(ep, old_block, (const uint8_t *)old_eh);
    if (ret < 0) {
        kfree(new_buf);
        return ret;
    }

    *new_eh = new_header;
    *new_block = alloc_block;

    kfree(new_buf);
    return 0;
}

/* ── Core extent tree traversal ────────────────────────────────────── */

/*
 * Walk an ext4 extent tree from root (stored in i_block[]) to leaf,
 * resolving @iblock to a physical block number.
 *
 * Algorithm:
 *   1. Copy the root node from i_block[0..14] (60 bytes) into a local buffer.
 *   2. Validate the root header.
 *   3. While depth > 0: binary search for the index entry, read child block.
 *   4. At depth 0: binary search for the extent covering iblock.
 *   5. Compute physical block from extent base + offset.
 *
 * Returns the physical block number on success, 0 for a hole, -1 on error.
 */
int64_t ext4_ext_find_extent(struct ext4_priv *ep,
                              struct ext4_inode *inode,
                              uint32_t iblock)
{
    /* The extent tree root is embedded in i_block[0..14].
     * struct ext4_extent_header is 12 bytes, so the name "60 bytes" comes
     * from 15 uint32_t entries (i_block[0..14]) minus the 12-byte header
     * space.  We copy the full 60 bytes to avoid alignment issues. */
    uint8_t root_buf[60];
    uint8_t node_buf[EXT4_MAX_BLOCK_SIZE];

    memcpy(root_buf, inode->i_block, 60);

    struct ext4_extent_header *eh = (struct ext4_extent_header *)root_buf;
    uint8_t *node_data = root_buf;

    /* Validate root header */
    int ret = ext4_ext_check_header(eh, EXT4_EXTENT_MAX_DEPTH);
    if (ret < 0) {
        kprintf("[ext4_ext] root extent header corrupt for inode\n");
        return ext4_corrupt(ep, "bad extent header in inode");
    }

    uint16_t depth = eh->eh_depth;

    /* Walk the tree from root to leaf */
    for (;;) {
        eh = (struct ext4_extent_header *)node_data;

        if (depth > 0) {
            /* ── Internal (index) node ── */
            struct ext4_extent_idx *idx_entry = NULL;

            ret = ext4_ext_binsearch_idx(eh, iblock, &idx_entry);
            if (ret < 0 || !idx_entry) {
                /* Before the first index entry — treat as hole */
                return 0;
            }

            /* Compute physical block of the child node */
            uint64_t child_block = ((uint64_t)idx_entry->ei_leaf_hi << 32) |
                                    idx_entry->ei_leaf_lo;

            if (child_block == 0) {
                /* Index entry pointing to block 0 — should not happen */
                kprintf("[ext4_ext] index entry points to block 0 (lblock=%u)\n",
                        idx_entry->ei_block);
                return 0;
            }

            /* Read the child block */
            if (ext4_read_block(ep, (uint32_t)child_block, node_buf) < 0)
                return ext4_corrupt(ep, "failed to read extent index block");

            node_data = node_buf;
            depth--;
        } else {
            /* ── Leaf node — search extents ── */
            struct ext4_extent *ext_entry = NULL;

            ret = ext4_ext_binsearch(eh, iblock, &ext_entry);
            if (ret < 0 || !ext_entry) {
                /* Not found in any extent — hole */
                return 0;
            }

            uint16_t len = ext_entry->ee_len;

            /* Check for uninitialized extent (bit 15 set) */
            if (len & 0x8000) {
                /* Uninitialized extents are treated as holes (return 0) */
                return 0;
            }

            /* Compute physical block */
            uint64_t phys = ((uint64_t)ext_entry->ee_start_hi << 32) |
                             ext_entry->ee_start_lo;
            phys += (iblock - ext_entry->ee_block);

            return (int64_t)phys;
        }
    }
}

/* ── Extended block resolution (with contiguous-block info) ────────── */

/*
 * Like ext4_ext_find_extent but also returns:
 *   - The number of contiguous blocks starting at iblock (max_blocks).
 *   - Whether the extent is uninitialized.
 *   - The physical block number (via phys_block pointer).
 *
 * This is the more general interface that will be needed for write
 * operations and journaling (Tasks 4, 9+).
 */
int ext4_ext_get_blocks(struct ext4_priv *ep,
                        struct ext4_inode *inode,
                        uint32_t iblock,
                        uint32_t *max_blocks,
                        uint64_t *phys_block,
                        int *uninit)
{
    if (!ep || !inode || !phys_block || !max_blocks)
        return -EINVAL;

    *phys_block = 0;
    if (uninit)
        *uninit = 0;

    if (!(inode->i_flags & EXT4_EXTENTS_FL))
        return -EINVAL;

    /* Build the root buffer */
    uint8_t root_buf[60];
    uint8_t node_buf[EXT4_MAX_BLOCK_SIZE];

    memcpy(root_buf, inode->i_block, 60);

    struct ext4_extent_header *eh = (struct ext4_extent_header *)root_buf;
    uint8_t *node_data = root_buf;

    int ret = ext4_ext_check_header(eh, EXT4_EXTENT_MAX_DEPTH);
    if (ret < 0) {
        kprintf("[ext4_ext] get_blocks: corrupt extent header\n");
        ext4_corrupt(ep, "bad extent header in get_blocks");
        return -EFSCORRUPTED;
    }

    uint16_t depth = eh->eh_depth;

    /* Walk the tree */
    for (;;) {
        eh = (struct ext4_extent_header *)node_data;

        if (depth > 0) {
            struct ext4_extent_idx *idx_entry = NULL;

            ret = ext4_ext_binsearch_idx(eh, iblock, &idx_entry);
            if (ret < 0 || !idx_entry)
                return 0; /* hole before first index entry */

            uint64_t child_block = ((uint64_t)idx_entry->ei_leaf_hi << 32) |
                                    idx_entry->ei_leaf_lo;

            if (child_block == 0)
                return 0;

            if (ext4_read_block(ep, (uint32_t)child_block, node_buf) < 0) {
                ext4_corrupt(ep, "get_blocks: failed to read index block");
                return -EFSCORRUPTED;
            }
            node_data = node_buf;
            depth--;
        } else {
            struct ext4_extent *ext_entry = NULL;

            ret = ext4_ext_binsearch(eh, iblock, &ext_entry);
            if (ret < 0 || !ext_entry)
                return 0; /* hole */

            uint16_t len = ext_entry->ee_len;
            int is_uninit = 0;

            if (len & 0x8000) {
                is_uninit = 1;
                len &= ~0x8000;
            }

            /* Fix for len == 0: treat as 32768 (full range) */
            if (len == 0)
                len = 32768;

            /* Compute contiguous remaining blocks without overflow.
             * binsearch guarantees iblock - ee_block < len, so the
             * subtraction is well-defined even when ee_block + len
             * would wrap past UINT32_MAX. */
            uint32_t offset = iblock - ext_entry->ee_block;
            uint32_t avail_blocks = len - offset;

            if (*max_blocks > avail_blocks)
                *max_blocks = avail_blocks;

            if (is_uninit) {
                if (uninit)
                    *uninit = 1;
                return 0; /* no physical blocks to return */
            }

            uint64_t phys = ((uint64_t)ext_entry->ee_start_hi << 32) |
                             ext_entry->ee_start_lo;
            phys += (iblock - ext_entry->ee_block);
            *phys_block = phys;

            return 0;
        }
    }
}

/* ── Insert extent into tree ───────────────────────────────────────── */

/*
 * ext4_ext_insert_extent — insert a new extent into the extent tree.
 *
 * @ep:     ext4 private per-mount data
 * @inode:  inode to modify (must have EXT4_EXTENTS_FL set)
 * @newext: the new extent to insert
 *
 * This is the main API for extent insertion.  The function:
 *   1. Traverses the tree to find the target leaf.
 *   2. Attempts to merge with adjacent extents.
 *   3. If the leaf has space, inserts directly.
 *   4. If the leaf is full, splits it.
 *   5. Updates the inode's i_block[] with any root changes.
 *
 * Returns 0 on success, negative errno on failure.
 */
int ext4_ext_insert_extent(struct ext4_priv *ep,
                           struct ext4_inode *inode,
                           struct ext4_extent *newext)
{
    int ret;

    if (!ep || !inode || !newext)
        return -EINVAL;

    if (!(inode->i_flags & EXT4_EXTENTS_FL))
        return -EINVAL;

    /* Validate the new extent */
    uint16_t new_len = newext->ee_len & ~0x8000;
    if (new_len == 0 || new_len > EXT4_EXT_MAX_LEN)
        return -EINVAL;

    if (newext->ee_start_lo == 0 &&
        newext->ee_start_hi == 0)
        return -EINVAL;

    /* ── Copy the root from the inode ── */
    uint8_t root_buf[60];
    uint8_t node_buf[EXT4_MAX_BLOCK_SIZE];

    memcpy(root_buf, inode->i_block, 60);

    struct ext4_extent_header *root_eh =
        (struct ext4_extent_header *)root_buf;
    uint8_t *node_data = root_buf;

    ret = ext4_ext_check_header(root_eh, EXT4_EXTENT_MAX_DEPTH);
    if (ret < 0)
        return -EFSCORRUPTED;

    uint16_t depth = root_eh->eh_depth;

    /* ── Walk to the leaf node ── */
    uint32_t path_block[EXT4_EXTENT_MAX_DEPTH + 1];
    struct ext4_extent_header *path_eh[EXT4_EXTENT_MAX_DEPTH + 1];
    int path_depth = 0;

    path_eh[0] = root_eh;
    path_block[0] = 0; /* root is embedded, not a real block */

    for (;;) {
        struct ext4_extent_header *eh =
            (struct ext4_extent_header *)node_data;

        if (eh->eh_depth > 0) {
            /* ── Internal node ── */
            struct ext4_extent_idx *idx_entry = NULL;

            ret = ext4_ext_binsearch_idx(eh, newext->ee_block, &idx_entry);
            if (ret < 0 || !idx_entry) {
                /* No appropriate index — should not happen with a valid
                 * tree but handle gracefully.  Insert at first child. */
                struct ext4_extent_idx *idx_arr =
                    (struct ext4_extent_idx *)(eh + 1);
                idx_entry = &idx_arr[0];
            }

            uint64_t child_block =
                ((uint64_t)idx_entry->ei_leaf_hi << 32) |
                idx_entry->ei_leaf_lo;

            if (child_block == 0)
                return -EFSCORRUPTED;

            uint32_t child_blk = (uint32_t)child_block;

            ret = ext4_read_block(ep, child_blk, node_buf);
            if (ret < 0)
                return -EFSCORRUPTED;

            path_depth++;
            path_eh[path_depth] = (struct ext4_extent_header *)node_data;
            path_block[path_depth] = child_blk;
            node_data = node_buf;
        } else {
            /* ── Leaf node — we're here ── */
            break;
        }
    }

    struct ext4_extent_header *leaf_eh =
        (struct ext4_extent_header *)node_data;

    /* If journaling, register the leaf block before any modifications */
    if (path_block[path_depth] != 0) {
        ret = ext4_ext_journal_get_write_access(ep,
                  path_block[path_depth], node_buf);
        if (ret < 0)
            return ret;
    }

    /* ── Try to merge the new extent with existing extents ── */
    struct ext4_extent *exts = (struct ext4_extent *)(leaf_eh + 1);
    uint16_t leaf_entries = leaf_eh->eh_entries;
    int merged = 0;
    int i;

    /* Check if we can merge with a preceding extent first, then
     * optionally the following extent after that merge. */
    for (i = 0; i < (int)leaf_entries; i++) {
        uint16_t ext_len = exts[i].ee_len & ~0x8000;
        if (ext_len == 0)
            ext_len = 32768;

        /* Avoid overflow when ee_block + ext_len wraps past 0xFFFFFFFF */
        uint32_t ext_end = (ext_len > 0xFFFFFFFFU - exts[i].ee_block)
                           ? 0xFFFFFFFFU
                           : exts[i].ee_block + ext_len;

        /* Does the new extent append directly after ext[i]? */
        if (ext_end == newext->ee_block) {
            /* Check physical contiguity */
            uint64_t phys_end =
                ((uint64_t)exts[i].ee_start_hi << 32) |
                exts[i].ee_start_lo;
            phys_end += ext_len;

            uint64_t new_phys =
                ((uint64_t)newext->ee_start_hi << 32) |
                newext->ee_start_lo;

            if (phys_end == new_phys) {
                /* Check combined length */
                uint32_t combined = ext_len + new_len;
                if (combined <= EXT4_EXT_MAX_LEN &&
                    !(exts[i].ee_len & 0x8000)) {
                    /* Extend existing extent */
                    uint16_t new_ee_len = (uint16_t)combined;
                    exts[i].ee_len = new_ee_len;
                    merged = 1;
                    break;
                }
            }
        }

        /* Does the new extent prepend before ext[i]? */
        if (newext->ee_block + new_len == exts[i].ee_block) {
            uint64_t new_phys =
                ((uint64_t)newext->ee_start_hi << 32) |
                newext->ee_start_lo;

            uint64_t ext_phys =
                ((uint64_t)exts[i].ee_start_hi << 32) |
                exts[i].ee_start_lo;

            if (new_phys + new_len == ext_phys) {
                uint32_t combined = ext_len + new_len;
                if (combined <= EXT4_EXT_MAX_LEN &&
                    !(exts[i].ee_len & 0x8000)) {
                    /* Extend existing extent backward */
                    exts[i].ee_block = newext->ee_block;
                    exts[i].ee_start_lo = newext->ee_start_lo;
                    exts[i].ee_start_hi = newext->ee_start_hi;
                    exts[i].ee_len = (uint16_t)combined;
                    merged = 1;
                    break;
                }
            }
        }
    }

    if (merged) {
        /* Try to merge again after extension */
        ext4_ext_merge(leaf_eh);

        /* Write back the leaf block if it's a real block */
        if (path_block[path_depth] != 0) {
            ret = ext4_ext_write_block(ep, path_block[path_depth],
                                       (const uint8_t *)leaf_eh);
            if (ret < 0)
                return ret;
        }

        /* Copy root back to inode if depth == 0 (root is the leaf) */
        if (depth == 0)
            memcpy(inode->i_block, root_buf, 60);

        return 0;
    }

    /* ── Find insertion position ── */
    int pos = 0;
    ret = ext4_ext_find_insert_position(leaf_eh, newext, &pos);
    if (ret < 0)
        return -EEXIST; /* overlap */

    /* ── Insert into leaf, splitting if necessary ── */
    if (leaf_eh->eh_entries < leaf_eh->eh_max) {
        /* Simple case: there's room in the leaf */
        ret = ext4_ext_insert_in_a_block(leaf_eh, newext, pos);
        if (ret < 0)
            return ret;

        /* Try to merge */
        ext4_ext_merge(leaf_eh);

        /* Write back leaf if it's a real block */
        if (path_block[path_depth] != 0) {
            ret = ext4_ext_write_block(ep, path_block[path_depth],
                                       (const uint8_t *)leaf_eh);
            if (ret < 0)
                return ret;
        }
    } else {
        /* ── Leaf is full: need to split ── */
        struct ext4_extent_header *new_eh = NULL;
        uint32_t new_block = 0;

        if (path_block[path_depth] == 0) {
            /* Root is embedded and it's full — need to grow the tree.
             * For now, return ENOSPC since tree growth requires
             * a block allocator and is a more advanced operation. */
            return -ENOSPC;
        }

        ret = ext4_ext_split(ep, leaf_eh, path_block[path_depth],
                             &new_eh, &new_block, 0);
        if (ret < 0)
            return ret;

        /* Determine which half the new extent goes into */
        struct ext4_extent *left_exts =
            (struct ext4_extent *)(leaf_eh + 1);
        struct ext4_extent *right_exts =
            (struct ext4_extent *)(new_eh + 1);
        uint16_t left_count = leaf_eh->eh_entries;
        uint16_t right_count = new_eh->eh_entries;

        struct ext4_extent_header *target_eh = NULL;
        uint32_t target_block = 0;
        int target_pos = 0;

        if (pos <= (int)left_count ||
            (left_count == 0)) {
            /* Goes in the left (old) node */
            target_eh = leaf_eh;
            target_block = path_block[path_depth];

            if (leaf_eh->eh_entries < leaf_eh->eh_max) {
                ret = ext4_ext_insert_in_a_block(target_eh, newext, pos);
                if (ret < 0)
                    return ret;

                ret = ext4_ext_write_block(ep, target_block,
                                           (const uint8_t *)target_eh);
                if (ret < 0)
                    return ret;
            } else {
                return -ENOSPC;
            }
        } else {
            /* Goes in the right (new) node */
            target_eh = new_eh;
            target_block = new_block;
            target_pos = pos - (int)left_count;

            if (target_pos > (int)right_count)
                target_pos = right_count;

            if (new_eh->eh_entries < new_eh->eh_max) {
                ret = ext4_ext_insert_in_a_block(target_eh, newext,
                                                 target_pos);
                if (ret < 0)
                    return ret;

                ret = ext4_ext_write_block(ep, target_block,
                                           (const uint8_t *)target_eh);
                if (ret < 0)
                    return ret;
            } else {
                return -ENOSPC;
            }
        }

        /* ── Update parent index entry ── */
        /* The old node (leaf_eh) has been truncated; the new node
         * (new_eh) has the upper half.  We need to insert an index
         * entry for the new node in the parent.
         *
         * For now, this is a simplified update that assumes the tree
         * is only one level deep (depth=1).  Recursive splits on
         * internal nodes are deferred to a future task. */
        if (path_depth > 0) {
            struct ext4_extent_header *parent_eh = path_eh[path_depth - 1];
            uint32_t parent_block = path_block[path_depth - 1];

            /* If journaling, register the parent block before modifying it */
            {
                uint8_t parent_buf[EXT4_MAX_BLOCK_SIZE];
                ret = ext4_read_block(ep, parent_block, parent_buf);
                if (ret < 0)
                    return ret;
                ret = ext4_ext_journal_get_write_access(ep, parent_block,
                                                         parent_buf);
                if (ret < 0)
                    return ret;
            }

            /* Build an index entry for the new node */
            struct ext4_extent_idx new_idx;
            memset(&new_idx, 0, sizeof(new_idx));

            /* The index key is the first logical block in the new node */
            if (right_count > 0) {
                new_idx.ei_block = right_exts[0].ee_block;
            } else {
                new_idx.ei_block = newext->ee_block;
            }
            new_idx.ei_leaf_lo = new_block;
            new_idx.ei_leaf_hi = 0;

            /* Find insertion position in the parent */
            int parent_pos = 0;
            struct ext4_extent_idx *parent_idx =
                (struct ext4_extent_idx *)(parent_eh + 1);
            uint16_t parent_entries = parent_eh->eh_entries;

            for (i = 0; i < (int)parent_entries; i++) {
                if (new_idx.ei_block < parent_idx[i].ei_block) {
                    parent_pos = i;
                    break;
                }
                parent_pos = i + 1;
            }

            /* Insert in parent */
            if (parent_eh->eh_entries >= parent_eh->eh_max) {
                /* Parent is also full — need recursive split.
                 * Deferred to a more advanced task. */
                return -ENOSPC;
            }

            if (parent_pos < (int)parent_entries) {
                memmove(&parent_idx[parent_pos + 1],
                        &parent_idx[parent_pos],
                        (parent_entries - parent_pos) *
                        sizeof(struct ext4_extent_idx));
            }
            memcpy(&parent_idx[parent_pos], &new_idx,
                   sizeof(struct ext4_extent_idx));
            parent_eh->eh_entries++;

            ret = ext4_ext_write_block(ep, parent_block,
                                       (const uint8_t *)parent_eh);
            if (ret < 0)
                return ret;
        }
    }

    /* ── Copy root back to inode if depth == 0 ── */
    if (depth == 0)
        memcpy(inode->i_block, root_buf, 60);

    return 0;
}

/* ── Remove extent space (punch hole) ──────────────────────────────── */

/*
 * ext4_ext_remove_space — remove all blocks in the range [start, end)
 * from the extent tree.
 *
 * @ep:     ext4 private per-mount data
 * @inode:  inode to modify
 * @start:  first logical block to remove
 * @end:    first logical block NOT to remove (i.e., blocks start..end-1)
 *
 * This is used for hole-punching (fallocate FALLOC_FL_PUNCH_HOLE) and
 * file truncation.  It:
 *   1. Traverses the extent tree to find affected extents.
 *   2. Truncates partially-overlapping extents (split).
 *   3. Removes fully-covered extents.
 *   4. Shifts remaining extents and updates the tree.
 *
 * Returns 0 on success, negative errno on failure.
 */
int ext4_ext_remove_space(struct ext4_priv *ep,
                          struct ext4_inode *inode,
                          uint32_t start,
                          uint32_t end)
{
    int ret;

    if (!ep || !inode)
        return -EINVAL;

    if (start >= end)
        return 0; /* nothing to remove */

    if (!(inode->i_flags & EXT4_EXTENTS_FL))
        return -EINVAL;

    /* ── Copy the root from the inode ── */
    uint8_t root_buf[60];
    uint8_t node_buf[EXT4_MAX_BLOCK_SIZE];

    memcpy(root_buf, inode->i_block, 60);

    struct ext4_extent_header *root_eh =
        (struct ext4_extent_header *)root_buf;
    struct ext4_extent_header *leaf_eh = root_eh;
    uint32_t leaf_block = 0;

    /* Walk tree to find the leaf containing the range */
    uint8_t *node_data = root_buf;

    for (;;) {
        struct ext4_extent_header *eh =
            (struct ext4_extent_header *)node_data;

        ret = ext4_ext_check_header(eh, EXT4_EXTENT_MAX_DEPTH);
        if (ret < 0)
            return -EFSCORRUPTED;

        if (eh->eh_depth > 0) {
            struct ext4_extent_idx *idx_entry = NULL;

            ret = ext4_ext_binsearch_idx(eh, start, &idx_entry);
            if (ret < 0 || !idx_entry) {
                /* No index for this range — nothing to remove */
                return 0;
            }

            uint64_t child_block =
                ((uint64_t)idx_entry->ei_leaf_hi << 32) |
                idx_entry->ei_leaf_lo;

            if (child_block == 0)
                return 0;

            leaf_block = (uint32_t)child_block;

            ret = ext4_read_block(ep, leaf_block, node_buf);
            if (ret < 0)
                return -EFSCORRUPTED;

            node_data = node_buf;
            leaf_eh = (struct ext4_extent_header *)node_data;
        } else {
            leaf_eh = eh;
            break;
        }
    }

    /* If journaling, register the leaf block before modifying it */
    if (leaf_block != 0) {
        ret = ext4_ext_journal_get_write_access(ep, leaf_block,
                                                 node_buf);
        if (ret < 0)
            return ret;
    }

    /* ── Process the leaf node ── */
    struct ext4_extent *exts = (struct ext4_extent *)(leaf_eh + 1);
    uint16_t entries = leaf_eh->eh_entries;
    int modified = 0;
    uint16_t new_entries = 0;

    /* We build a new extent array in-place, moving surviving
     * (or truncated) extents to the front.
     *
     * For each existing extent:
     *   - If entirely outside [start, end): keep it.
     *   - If entirely inside [start, end): drop it.
     *   - If partially overlapping: truncate the overlapping part. */
    for (uint16_t i = 0; i < entries; i++) {
        uint16_t ext_len = exts[i].ee_len & ~0x8000;
        if (ext_len == 0)
            ext_len = 32768;

        uint32_t ext_start = exts[i].ee_block;
        /* Avoid overflow when ext_start + ext_len wraps past 0xFFFFFFFF */
        uint32_t ext_end = (ext_len > 0xFFFFFFFFU - ext_start)
                           ? 0xFFFFFFFFU
                           : ext_start + ext_len;
        int uninit = !!(exts[i].ee_len & 0x8000);

        /* Case 1: Entirely before the range — keep */
        if (ext_end <= start) {
            if (i != new_entries)
                exts[new_entries] = exts[i];
            new_entries++;
            continue;
        }

        /* Case 2: Entirely after the range — keep */
        if (ext_start >= end) {
            if (i != new_entries)
                exts[new_entries] = exts[i];
            new_entries++;
            continue;
        }

        /* Case 3: Entirely within the range — drop */
        if (ext_start >= start && ext_end <= end) {
            modified = 1;
            continue;
        }

        /* Case 4: Extent starts before range — truncate tail */
        if (ext_start < start && ext_end > start && ext_end <= end) {
            uint16_t new_len_val = (uint16_t)(start - ext_start);
            if (uninit)
                exts[i].ee_len = new_len_val | 0x8000;
            else
                exts[i].ee_len = new_len_val;

            if (i != new_entries)
                exts[new_entries] = exts[i];
            new_entries++;
            modified = 1;
            continue;
        }

        /* Case 5: Extent starts within range — truncate head */
        if (ext_start >= start && ext_start < end && ext_end > end) {
            /* Update ee_block and ee_start to reflect truncation */
            uint32_t overlap = end - ext_start;
            exts[i].ee_block = end;

            /* Update physical start */
            uint64_t phys = ((uint64_t)exts[i].ee_start_hi << 32) |
                             exts[i].ee_start_lo;
            phys += overlap;
            exts[i].ee_start_lo = (uint32_t)(phys & 0xFFFFFFFF);
            exts[i].ee_start_hi = (uint16_t)(phys >> 32);

            uint16_t new_len_val = (uint16_t)(ext_end - end);
            if (new_len_val == 0)
                continue; /* extent fully consumed — drop */
            if (uninit)
                exts[i].ee_len = new_len_val | 0x8000;
            else
                exts[i].ee_len = new_len_val;

            if (i != new_entries)
                exts[new_entries] = exts[i];
            new_entries++;
            modified = 1;
            continue;
        }

        /* Case 6: Extent spans across the range entirely — split */
        if (ext_start < start && ext_end > end) {
            /* We need to create two extents from one.
             * Left part: [ext_start, start)
             * Right part: [end, ext_end) */

            /* Modify the current extent to be the left part */
            uint16_t left_len = (uint16_t)(start - ext_start);
            if (uninit)
                exts[i].ee_len = left_len | 0x8000;
            else
                exts[i].ee_len = left_len;

            if (i != new_entries)
                exts[new_entries] = exts[i];
            new_entries++;
            modified = 1;

            /* Create a new entry for the right part.
             * We use the last slot to add the right part,
             * but only if there's room (rare but possible). */
            uint32_t right_start = end;
            uint16_t right_len = (uint16_t)(ext_end - end);
            if (right_len > 0) {
                uint64_t right_phys =
                    ((uint64_t)exts[i].ee_start_hi << 32) |
                    exts[i].ee_start_lo;
                right_phys += (right_start - ext_start);

                if (new_entries < leaf_eh->eh_max) {
                    /* Move existing entries to make room */
                    struct ext4_extent *dst = &exts[new_entries];
                    dst->ee_block = right_start;
                    if (uninit)
                        dst->ee_len = right_len | 0x8000;
                    else
                        dst->ee_len = right_len;
                    dst->ee_start_lo = (uint32_t)(right_phys & 0xFFFFFFFF);
                    dst->ee_start_hi = (uint16_t)(right_phys >> 32);
                    new_entries++;
                }
                /* If no room, the right part is dropped (data loss
                 * prevention requires proper split — defer for now) */
            }
            continue;
        }
    }

    if (!modified)
        return 0;

    leaf_eh->eh_entries = new_entries;

    /* Try to merge after removal */
    ext4_ext_merge(leaf_eh);

    /* Write back the leaf block if modified */
    if (leaf_block != 0) {
        ret = ext4_ext_write_block(ep, leaf_block,
                                   (const uint8_t *)leaf_eh);
        if (ret < 0)
            return ret;
    }

    /* If root was modified (depth == 0 leaf), copy back */
    if (root_eh->eh_depth == 0)
        memcpy(inode->i_block, root_buf, 60);

    return 0;
}

/* ── Module init ───────────────────────────────────────────────────── */

int __init ext4_ext_init(void)
{
    kprintf("[ext4_ext] extent tree module initialized\n");
    return 0;
}

#ifdef MODULE
int __init init_module(void) { return ext4_ext_init(); }
void __exit cleanup_module(void) {}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Ext4 extent tree traversal and management");
MODULE_VERSION("1.0");
#endif
