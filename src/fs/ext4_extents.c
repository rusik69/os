/*
 * src/fs/ext4_extents.c — Ext4 extent tree traversal and management.
 *
 * Implements the extent tree walk used by ext4 to resolve logical block
 * numbers to physical block numbers on disk.  Supports multi-level trees
 * up to EXT4_EXTENT_MAX_DEPTH with binary search at each internal node.
 *
 * Part of the Hermes OS ext4 filesystem implementation.
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
    uint32_t entry_size;

    if (depth == 0)
        entry_size = sizeof(struct ext4_extent);
    else
        entry_size = sizeof(struct ext4_extent_idx);

    uint32_t avail = block_size - sizeof(struct ext4_extent_header);
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
                len = 32768; /* treat 0 len as full range (convention) */
            if (iblock < ext[mid].ee_block + len) {
                if (ret_ext)
                    *ret_ext = &ext[mid];
                return 0;
            }
            lo = mid + 1;
        }
    }

    return -1; /* not found — hole */
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

            uint32_t extent_end = ext_entry->ee_block + len;
            uint32_t avail_blocks = extent_end - iblock;

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
