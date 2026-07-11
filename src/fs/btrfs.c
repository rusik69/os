/*
 * src/fs/btrfs.c — Btrfs read-only filesystem
 *
 * Implements a read-only, single-device, non-raid, non-compressed Btrfs.
 * Supports: stat, readdir, read (inline and extent data).
 */

#define KERNEL_INTERNAL
#include "btrfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"
#include "blockdev.h"
#include "types.h"
#include "crc.h"

#ifdef MODULE
#include "module.h"
#endif
#include "initcall.h"

/* ── Utilities ────────────────────────────────────────────────── */

static inline uint64_t le64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* ── Block I/O ────────────────────────────────────────────────── */

static int btrfs_read_blocks(struct btrfs_priv *bp, uint64_t bytenr,
                              uint32_t count, uint8_t *buf)
{
    /* bytenr is physical byte offset on device */
    uint64_t lba = bytenr / 512;
    uint32_t sectors = count * 512 / 512; /* count is in 512-byte sectors */
    /* but count may be in nodesize/sectorsize units */
    for (uint32_t i = 0; i < count; i++) {
        if (blockdev_read_sectors(bp->dev_id, (uint32_t)(lba + i), 1,
                                   buf + i * 512) != 0)
            return -1;
    }
    return 0;
}

static int btrfs_read_node(struct btrfs_priv *bp, uint64_t bytenr,
                            uint8_t *buf)
{
    uint32_t sectors = bp->nodesize / 512;
    uint64_t lba = bytenr / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(bp->dev_id, (uint32_t)(lba + i), 1,
                                   buf + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* ── Superblock parsing ───────────────────────────────────────── */

/**
 * btrfs_parse_superblock - Read, validate, and parse the Btrfs superblock
 * @bp: Btrfs private data (dev_id must be set)
 *
 * Reads the primary superblock at offset BTRFS_SUPER_OFFSET (0x10000),
 * validates the magic, performs sanity checks on key fields, and
 * populates the btrfs_priv structure.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int btrfs_parse_superblock(struct btrfs_priv *bp)
{
    uint8_t buf[BTRFS_SB_SIZE];
    uint64_t sb_lba = BTRFS_SUPER_OFFSET / 512;
    uint8_t dev_id = bp->dev_id;

    /* Read 8 sectors (4096 bytes) for the primary superblock */
    for (uint32_t i = 0; i < 8; i++) {
        int ret = blockdev_read_sectors(dev_id, (uint32_t)(sb_lba + i), 1,
                                         buf + i * 512);
        if (ret < 0)
            return -EIO;
    }

    /* Validate magic at offset 0x40 within the superblock */
    static const uint8_t expected_magic[BTRFS_MAGIC_SIZE] = {
        0x5F, 0x42, 0x48, 0x52, 0x66, 0x53, 0x5F, 0x4D  /* "_BHRfS_M" */
    };
    if (memcmp(buf + 0x40, expected_magic, BTRFS_MAGIC_SIZE) != 0)
        return -EINVAL;

    /* Cast to superblock structure */
    struct btrfs_superblock *sb = (struct btrfs_superblock *)buf;

    /* Sanity-check critical geometry fields */
    if (sb->sectorsize == 0 || sb->nodesize == 0)
        return -EINVAL;
    if (sb->sectorsize > 65536 || sb->nodesize > 65536)
        return -EINVAL;
    if ((sb->sectorsize & (sb->sectorsize - 1U)) != 0U)
        return -EINVAL;  /* not a power of 2 */
    if ((sb->nodesize & (sb->nodesize - 1U)) != 0U)
        return -EINVAL;

    /* Verify tree root node addresses are non-zero */
    if (sb->root == 0)
        return -EINVAL;
    if (sb->chunk_root == 0)
        return -EINVAL;

    /* Check incompatible features — we only support MIXED_BACKREF (bit 0) */
    if (sb->incompat_flags & ~(1ULL << 0))
        return -EINVAL;

    /* Populate btrfs_priv from superblock fields */
    bp->sectorsize         = sb->sectorsize;
    bp->nodesize           = sb->nodesize;
    bp->csum_type          = sb->csum_type;
    bp->chunk_root_bytenr  = sb->chunk_root;
    bp->chunk_root_level   = sb->chunk_root_level;
    bp->root_bytenr        = sb->root;
    bp->root_level         = sb->root_level;
    bp->num_chunks         = 0;

    kprintf("[btrfs] superblock: gen=%llu, root=0x%llx, "
            "chunk_root=0x%llx, total=%llu, "
            "sectorsize=%u, nodesize=%u\n",
            (unsigned long long)sb->generation,
            (unsigned long long)sb->root,
            (unsigned long long)sb->chunk_root,
            (unsigned long long)sb->total_bytes,
            sb->sectorsize, sb->nodesize);

    return 0;
}

/* ── Logical to physical translation ──────────────────────────── */

static int btrfs_chunk_map(struct btrfs_priv *bp, uint64_t logical,
                            uint64_t *physical)
{
    for (uint32_t i = 0; i < bp->num_chunks; i++) {
        if (logical >= bp->chunks[i].logical &&
            logical < bp->chunks[i].logical + bp->chunks[i].length) {
            *physical = bp->chunks[i].physical + (logical - bp->chunks[i].logical);
            return 0;
        }
    }
    return -1;
}

/* ── Tree traversal ───────────────────────────────────────────── */

struct btrfs_tree_search {
    uint64_t objectid;
    uint8_t  type;
    uint64_t offset;
    uint32_t item_index;
    void    *data;
    uint32_t data_size;
    int      found;
};

/* Read tree node/leaf and search for a key.
 * On success, fills item_index and data points into buf for leaf nodes. */
static int btrfs_search_tree(struct btrfs_priv *bp, uint64_t root_bytenr,
                              uint8_t root_level, uint64_t objectid,
                              uint8_t type, uint64_t offset,
                              uint8_t *buf, uint32_t buf_size,
                              uint32_t *item_idx, int *exact)
{
    uint64_t bytenr;

    /* Reject zero root bytenr — would read from physical LBA 0 */
    if (root_bytenr == 0)
        return -1;

    /* Translate logical -> physical for the root node */
    if (btrfs_chunk_map(bp, root_bytenr, &bytenr) < 0)
        bytenr = root_bytenr;
    uint8_t  level = root_level;
    *exact = 0;

    for (;;) {
        if (btrfs_read_node(bp, bytenr, buf) < 0)
            return -1;

        struct btrfs_header *hdr = (struct btrfs_header *)buf;
        uint32_t nritems = hdr->nritems;
        uint32_t hi = nritems;
        uint32_t lo = 0;
        uint32_t mid;
        int found = 0;

        /* Binary search for key */
        while (lo < hi) {
            mid = lo + (hi - lo) / 2;
            uint8_t *item_base;

            if (hdr->level == 0) {
                /* Leaf node: item headers at offset sizeof(header) */
                struct btrfs_item *items = (struct btrfs_item *)(buf + sizeof(struct btrfs_header));
                uint64_t obj = items[mid].key.objectid;
                uint8_t  typ = items[mid].key.type;
                uint64_t off = items[mid].key.offset;

                if (obj < objectid || (obj == objectid && typ < type) ||
                    (obj == objectid && typ == type && off < offset)) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            } else {
                /* Internal node: key pointers */
                struct btrfs_key_ptr *kptr = (struct btrfs_key_ptr *)
                    (buf + sizeof(struct btrfs_header));
                uint64_t obj = kptr[mid].key.objectid;
                uint8_t  typ = kptr[mid].key.type;
                uint64_t off = kptr[mid].key.offset;

                if (obj < objectid || (obj == objectid && typ < type) ||
                    (obj == objectid && typ == type && off < offset)) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
        }

        if (lo < nritems) {
            if (hdr->level == 0) {
                struct btrfs_item *items = (struct btrfs_item *)(buf + sizeof(struct btrfs_header));
                uint64_t obj = items[lo].key.objectid;
                uint8_t  typ = items[lo].key.type;
                uint64_t off = items[lo].key.offset;
                if (obj == objectid && typ == type && off == offset) {
                    *exact = 1;
                } else if (type == 0 && obj == objectid && typ == type) {
                    *exact = 1; /* offset don't care for some lookups */
                }
                *item_idx = lo;
                return 0;
            } else {
                /* Descend to child */
                struct btrfs_key_ptr *kptr = (struct btrfs_key_ptr *)
                    (buf + sizeof(struct btrfs_header));
                uint64_t child_bytenr = kptr[lo].blockptr;
                uint64_t child_gen = kptr[lo].generation;
                (void)child_gen;
                /* Read chunk map for logical->physical */
                if (btrfs_chunk_map(bp, child_bytenr, &bytenr) < 0) {
                    /* If linear mapping fails, assume identity */
                    bytenr = child_bytenr;
                }
                level--;
                continue;
            }
        } else {
            /* Past end */
            if (hdr->level == 0) {
                *item_idx = nritems;
                return 0;
            } else {
                /* Descend to last child */
                struct btrfs_key_ptr *kptr = (struct btrfs_key_ptr *)
                    (buf + sizeof(struct btrfs_header));
                if (nritems > 0) {
                    uint64_t child_bytenr = kptr[nritems - 1].blockptr;
                    if (btrfs_chunk_map(bp, child_bytenr, &bytenr) < 0)
                        bytenr = child_bytenr;
                    level--;
                    continue;
                }
                *item_idx = 0;
                return 0;
            }
        }
    }
}

/* Read data of a leaf item into out buffer. */
static int btrfs_read_item_data(struct btrfs_priv *bp, uint64_t bytenr,
                                 uint8_t *node_buf, uint32_t item_idx,
                                 uint8_t *out, uint32_t *out_size)
{
    struct btrfs_header *hdr = (struct btrfs_header *)node_buf;
    struct btrfs_item *items = (struct btrfs_item *)(node_buf + sizeof(struct btrfs_header));

    if (item_idx >= hdr->nritems)
        return -1;

    uint32_t offset = items[item_idx].offset;
    uint32_t size = items[item_idx].size;
    if (offset > bp->nodesize || size > bp->nodesize - offset)
        return -1;

    memcpy(out, node_buf + offset, size);
    *out_size = size;
    return 0;
}

/* ── Walk chunk tree to build logical->physical mapping ───────── */

static int btrfs_build_chunk_tree(struct btrfs_priv *bp)
{
    uint8_t buf[4096]; /* must be >= nodesize */
    uint64_t search_objectid = 0;
    uint8_t  search_type = BTRFS_CHUNK_ITEM_KEY;
    uint64_t search_offset = 0;

    bp->num_chunks = 0;

    /* Iterate all items in the chunk tree via repeated key search.
     *
     * Btrfs nodes do not carry leaf sibling pointers, so we cannot
     * simply walk a leaf chain. Instead, we use a repeated-search
     * pattern: each call to btrfs_search_tree descends from the tree
     * root and finds the item whose key is >= the requested key.
     * By advancing the search key past every item we find, we
     * naturally walk the entire tree in key-sorted order.
     *
     * The chunk tree is small (typically < 50 items), so the O(n·log n)
     * cost of repeated searches is negligible.
     *
     * Bootstrapping: when the chunk map is still empty, btrfs_read_node
     * inside btrfs_search_tree will attempt identity mapping (logical ==
     * physical) via the fallback in btrfs_chunk_map.  For single-device,
     * non-RAID Btrfs the SYSTEM block group that holds the chunk tree is
     * identity-mapped, so this works. */
    while (bp->num_chunks < BTRFS_MAX_CHUNKS) {
        uint32_t item_idx;
        int exact;
        int ret;

        ret = btrfs_search_tree(bp, bp->chunk_root_bytenr,
                                 bp->chunk_root_level,
                                 search_objectid, search_type,
                                 search_offset,
                                 buf, sizeof(buf),
                                 &item_idx, &exact);
        if (ret < 0)
            break;

        struct btrfs_header *hdr = (struct btrfs_header *)buf;
        if (item_idx >= hdr->nritems)
            break;

        struct btrfs_item *items = (struct btrfs_item *)
            (buf + sizeof(struct btrfs_header));

        uint64_t cur_obj = items[item_idx].key.objectid;
        uint8_t  cur_typ = items[item_idx].key.type;
        uint64_t cur_off = items[item_idx].key.offset;

        /* Process chunk items; skip any other item types silently */
        if (cur_typ == BTRFS_CHUNK_ITEM_KEY) {
            uint32_t chk_off = items[item_idx].offset;
            uint32_t chk_sz  = items[item_idx].size;

            if (chk_off <= bp->nodesize && chk_sz <= bp->nodesize - chk_off &&
                chk_sz >= sizeof(struct btrfs_chunk)) {
                struct btrfs_chunk *chunk =
                    (struct btrfs_chunk *)(buf + chk_off);
                uint16_t num_stripes = chunk->num_stripes;

                /* Only single-device, non-RAID chunks */
                if (num_stripes == 1 &&
                    !(chunk->type & BTRFS_BLOCK_GROUP_RAID_MASK)) {
                    /* Verify the item is large enough for the stripe array.
                     * sizeof(struct btrfs_chunk) + 1 * sizeof(struct btrfs_stripe)
                     * is the minimum for a single-stripe chunk.  Without this
                     * check, a crafted chunk item with num_stripes==1 but
                     * undersized chk_sz would cause stripe->offset to read
                     * out-of-bounds item data within the node buffer. */
                    if (chk_sz < sizeof(struct btrfs_chunk) +
                                 sizeof(struct btrfs_stripe))
                        continue;

                    struct btrfs_stripe *stripe =
                        (struct btrfs_stripe *)
                            ((uint8_t *)chunk +
                             sizeof(struct btrfs_chunk));

                    bp->chunks[bp->num_chunks].logical  = cur_off;
                    bp->chunks[bp->num_chunks].length   = chunk->length;
                    bp->chunks[bp->num_chunks].physical = stripe->offset;
                    bp->num_chunks++;
                }
            }
        }

        /* Advance search past the current key */
        search_offset = cur_off + 1;
        search_type   = cur_typ;
        search_objectid = cur_obj;

        /* Handle offset / type / objectid wraparound */
        if (search_offset == 0) {
            search_type = cur_typ + 1;
            if (search_type == 0)
                search_objectid = cur_obj + 1;
        }
    }

    kprintf("[btrfs] built chunk map: %u entries (searched objectid=%llu "
            "type=%u offset=%llu)\n",
            bp->num_chunks,
            (unsigned long long)search_objectid,
            (unsigned)search_type,
            (unsigned long long)search_offset);
    return bp->num_chunks > 0 ? 0 : -1;
}

/* ── Superblock root backup ─────────────────────────────────────── */

/**
 * btrfs_read_root_backup - Read a root backup slot from the superblock
 * @bp: Btrfs private data
 * @slot: Backup slot index (0-3)
 * @rb: Output buffer for root backup structure
 *
 * Reads the superblock raw data and extracts the specified root backup
 * slot at offset BTRFS_ROOT_BACKUP_OFFSET (0x300) within the 4096-byte
 * superblock.  Slot 0 contains the current generation's tree root
 * addresses; slots 1-3 are older snapshots.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int btrfs_read_root_backup(struct btrfs_priv *bp, unsigned int slot,
                                   struct btrfs_root_backup *rb)
{
    uint8_t raw_sb[4096];
    uint64_t sb_lba = BTRFS_SUPER_OFFSET / 512;

    for (uint32_t k = 0; k < 8; k++) {
        int ret = blockdev_read_sectors(bp->dev_id, (uint32_t)(sb_lba + k), 1,
                                         raw_sb + k * 512);
        if (ret < 0)
            return -EIO;
    }

    if (slot >= BTRFS_NUM_ROOT_BACKUPS)
        return -EINVAL;

    uint64_t backup_offset = BTRFS_ROOT_BACKUP_OFFSET +
                             (uint64_t)slot * sizeof(struct btrfs_root_backup);
    if (backup_offset + sizeof(*rb) > sizeof(raw_sb))
        return -EINVAL;

    memcpy(rb, raw_sb + backup_offset, sizeof(*rb));
    return 0;
}

/* ── Root tree parsing ─────────────────────────────────────────── */

/**
 * btrfs_parse_root_tree - Parse root tree to locate FS tree subvolume
 * @bp: Btrfs private data
 *
 * Parsing strategy:
 *
 * The superblock stores logical addresses for the root tree and chunk
 * tree directly in its 'root' and 'chunk_root' fields.  The root tree
 * is a B-tree whose root node lives at that logical address.
 *
 * Within the root tree, every subvolume (tree) has a ROOT_ITEM entry
 * keyed by (objectid, ROOT_ITEM_KEY, transid).  The ROOT_ITEM stores
 * the tree's root_dirid and depth (level), but it does NOT store the
 * tree root node's logical address.
 *
 * To obtain the tree root logical address for each critical tree (FS,
 * extent, device, checksum), we read the ROOT_BACKUP array embedded in
 * the superblock at offset 0x300.  Slot 0 caches the addresses for the
 * current transaction's tree roots.  For a read-only, single-device,
 * non-RAID Btrfs this is sufficient for mounting.
 *
 * Once we have the FS tree root logical address, we translate it to
 * physical via the chunk tree, extract the level and root_dirid from
 * the ROOT_ITEM in the root tree, and store everything in btrfs_priv.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int btrfs_parse_root_tree(struct btrfs_priv *bp)
{
    struct btrfs_root_backup rb;
    int ret;

    /* Read slot 0 of the superblock root backups */
    ret = btrfs_read_root_backup(bp, 0, &rb);
    if (ret < 0) {
        kprintf("[btrfs] failed to read root backup: %d\n", ret);
        return ret;
    }

    /* Translate FS tree root logical address to physical via chunk map */
    uint64_t fs_root_logical = rb.fs_root_bytenr;
    if (fs_root_logical == 0) {
        kprintf("[btrfs] FS root bytenr is zero in root backup\n");
        return -EINVAL;
    }
    if (btrfs_chunk_map(bp, fs_root_logical, &bp->fs_root_bytenr) < 0) {
        kprintf("[btrfs] cannot map FS root logical 0x%llx\n",
                (unsigned long long)fs_root_logical);
        return -EINVAL;
    }

    /* Search root tree for FS_TREE ROOT_ITEM to get level & root_dirid */
    uint8_t tree_buf[4096];
    uint32_t ri_idx;
    int ri_exact;
    ret = btrfs_search_tree(bp, bp->root_bytenr, bp->root_level,
                             BTRFS_FS_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0,
                             tree_buf, sizeof(tree_buf), &ri_idx, &ri_exact);
    if (ret < 0) {
        kprintf("[btrfs] cannot search root tree: %d\n", ret);
        return ret;
    }

    if (ri_exact) {
        uint8_t ri_data[sizeof(struct btrfs_root_item) + 64];
        uint32_t ri_size;
        ret = btrfs_read_item_data(bp, bp->root_bytenr, tree_buf, ri_idx,
                                    ri_data, &ri_size);
        if (ret < 0) {
            kprintf("[btrfs] cannot read root item data: %d\n", ret);
            return ret;
        }
        if (ri_size < sizeof(struct btrfs_root_item)) {
            kprintf("[btrfs] root item too small: %u bytes\n", ri_size);
            return -EINVAL;
        }
        struct btrfs_root_item *ritem = (struct btrfs_root_item *)ri_data;
        bp->fs_root_level  = ritem->level;
        bp->fs_root_dirid  = ritem->root_dirid;
    } else {
        /* Fall back to sensible defaults when root item not present */
        bp->fs_root_level = 0;
        bp->fs_root_dirid = BTRFS_FS_TREE_OBJECTID;
    }

    kprintf("[btrfs] FS root: bytenr=0x%llx, level=%u, dirid=%llu\n",
            (unsigned long long)bp->fs_root_bytenr,
            (unsigned)bp->fs_root_level,
            (unsigned long long)bp->fs_root_dirid);

    /* ── Resolve checksum tree root ────────────────────────────── */
    /* Translate csum root logical address from root backup */
    uint64_t csum_logical = rb.csum_root_bytenr;
    if (csum_logical != 0) {
        if (btrfs_chunk_map(bp, csum_logical, &bp->csum_root_bytenr) < 0) {
            kprintf("[btrfs] cannot map csum root logical 0x%llx\n",
                    (unsigned long long)csum_logical);
            bp->csum_root_bytenr = 0;
        } else {
            /* Look up CSUM_TREE root item in root tree for level */
            ret = btrfs_search_tree(bp, bp->root_bytenr, bp->root_level,
                                     BTRFS_CSUM_TREE_OBJECTID,
                                     BTRFS_ROOT_ITEM_KEY, 0,
                                     tree_buf, sizeof(tree_buf),
                                     &ri_idx, &ri_exact);
            if (ret == 0 && ri_exact) {
                uint8_t csum_ri_data[sizeof(struct btrfs_root_item) + 64];
                uint32_t csum_ri_size;
                ret = btrfs_read_item_data(bp, bp->root_bytenr, tree_buf,
                                            ri_idx, csum_ri_data,
                                            &csum_ri_size);
                if (ret == 0 &&
                    csum_ri_size >= sizeof(struct btrfs_root_item)) {
                    struct btrfs_root_item *csum_ritem =
                        (struct btrfs_root_item *)csum_ri_data;
                    bp->csum_root_level = csum_ritem->level;
                } else {
                    bp->csum_root_level = 0;
                }
            } else {
                bp->csum_root_level = 0;
            }

            kprintf("[btrfs] csum tree: bytenr=0x%llx, level=%u\n",
                    (unsigned long long)bp->csum_root_bytenr,
                    (unsigned)bp->csum_root_level);
        }
    } else {
        bp->csum_root_bytenr = 0;
        bp->csum_root_level = 0;
        kprintf("[btrfs] csum tree: not available\n");
    }

    return 0;
}

/* ── Extent tree parsing ──────────────────────────────────────── */

/**
 * btrfs_parse_extent_tree - Walk the extent tree and log extent items
 * @bp: Btrfs private data
 *
 * Reads the extent tree root location from the root backup (slot 0),
 * translates it through the chunk map, then walks the extent tree
 * logging every EXTENT_ITEM_KEY and METADATA_ITEM_KEY entry.
 *
 * Btrfs extent items track physical block allocations.  Each item's
 * key is (block_number, EXTENT_ITEM_KEY, length) for data extents or
 * (block_number, METADATA_ITEM_KEY, level) for tree metadata blocks.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int btrfs_parse_extent_tree(struct btrfs_priv *bp)
{
    struct btrfs_root_backup rb;
    uint8_t buf[4096];
    uint64_t extent_tree_logical;
    uint32_t item_idx;
    int exact;
    int ret;
    uint32_t extent_count;
    uint32_t metadata_count;

    ret = btrfs_read_root_backup(bp, 0, &rb);
    if (ret < 0) {
        kprintf("[btrfs] failed to read root backup for extent tree: %d\n",
                ret);
        return ret;
    }

    extent_tree_logical = rb.extent_root_bytenr;
    if (extent_tree_logical == 0) {
        kprintf("[btrfs] extent root bytenr is zero in root backup\n");
        return -EINVAL;
    }
    if (btrfs_chunk_map(bp, extent_tree_logical,
                         &bp->extent_root_bytenr) < 0) {
        kprintf("[btrfs] cannot map extent root logical 0x%llx\n",
                (unsigned long long)extent_tree_logical);
        return -EINVAL;
    }

    /* Search root tree for EXTENT_TREE root item to get level */
    ret = btrfs_search_tree(bp, bp->root_bytenr, bp->root_level,
                             BTRFS_EXTENT_TREE_OBJECTID,
                             BTRFS_ROOT_ITEM_KEY, 0,
                             buf, sizeof(buf), &item_idx, &exact);
    if (ret < 0) {
        kprintf("[btrfs] cannot search root tree for extent root: %d\n",
                ret);
        return ret;
    }

    if (exact) {
        uint8_t ri_data[sizeof(struct btrfs_root_item) + 64];
        uint32_t ri_size;
        ret = btrfs_read_item_data(bp, bp->root_bytenr, buf, item_idx,
                                    ri_data, &ri_size);
        if (ret == 0 &&
            ri_size >= sizeof(struct btrfs_root_item)) {
            struct btrfs_root_item *ritem =
                (struct btrfs_root_item *)ri_data;
            bp->extent_root_level = ritem->level;
        } else {
            bp->extent_root_level = 0;
        }
    } else {
        bp->extent_root_level = 0;
    }

    kprintf("[btrfs] extent tree: bytenr=0x%llx, level=%u\n",
            (unsigned long long)bp->extent_root_bytenr,
            (unsigned)bp->extent_root_level);

    /* Walk the extent tree and log extent items */
    extent_count = 0;
    metadata_count = 0;
    {
        uint64_t search_obj = 0;
        uint8_t  search_type = 0;
        uint64_t search_off = 0;

        while (extent_count + metadata_count < 1000) {
            ret = btrfs_search_tree(bp, bp->extent_root_bytenr,
                                     bp->extent_root_level,
                                     search_obj, search_type, search_off,
                                     buf, sizeof(buf),
                                     &item_idx, &exact);
            if (ret < 0)
                break;

            struct btrfs_header *hdr = (struct btrfs_header *)buf;
            if (item_idx >= hdr->nritems)
                break;

            struct btrfs_item *items = (struct btrfs_item *)
                (buf + sizeof(struct btrfs_header));

            uint64_t cur_obj = items[item_idx].key.objectid;
            uint8_t  cur_typ = items[item_idx].key.type;
            uint64_t cur_off = items[item_idx].key.offset;

            if (cur_typ == BTRFS_EXTENT_ITEM_KEY) {
                uint32_t off = items[item_idx].offset;
                uint32_t sz  = items[item_idx].size;
                if (off <= bp->nodesize && sz <= bp->nodesize - off &&
                    sz >= sizeof(struct btrfs_extent_item)) {
                    struct btrfs_extent_item *ei =
                        (struct btrfs_extent_item *)(buf + off);
                    kprintf("[btrfs]   extent 0x%llx len=%llu "
                            "refs=%llu gen=%llu flags=0x%llx\n",
                            (unsigned long long)cur_obj,
                            (unsigned long long)cur_off,
                            (unsigned long long)ei->refs,
                            (unsigned long long)ei->generation,
                            (unsigned long long)ei->flags);
                    extent_count++;
                }
            } else if (cur_typ == BTRFS_METADATA_ITEM_KEY) {
                uint32_t off = items[item_idx].offset;
                uint32_t sz  = items[item_idx].size;
                if (off <= bp->nodesize && sz <= bp->nodesize - off &&
                    sz >= sizeof(struct btrfs_extent_item)) {
                    struct btrfs_extent_item *ei =
                        (struct btrfs_extent_item *)(buf + off);
                    kprintf("[btrfs]   metadata 0x%llx "
                            "level=%llu refs=%llu gen=%llu flags=0x%llx\n",
                            (unsigned long long)cur_obj,
                            (unsigned long long)cur_off,
                            (unsigned long long)ei->refs,
                            (unsigned long long)ei->generation,
                            (unsigned long long)ei->flags);
                    metadata_count++;
                }
            }

            /* Advance search past this key */
            search_obj = cur_obj;
            search_type = cur_typ;
            search_off = cur_off + 1;
            if (search_off == 0) {
                search_type++;
                if (search_type == 0)
                    search_obj++;
            }
        }
    }

    kprintf("[btrfs] extent tree walk: %u extents, %u metadata items\n",
            extent_count, metadata_count);
    return 0;
}

/* ── Checksum tree parsing ────────────────────────────────────── */

/**
 * btrfs_parse_csum_tree - Walk the checksum tree and log entries
 * @bp: Btrfs private data (populated with csum_root_bytenr/level)
 *
 * Walks the checksum tree (CSUM_TREE_OBJECTID) in key order, logging
 * each checksum item's logical block range and number of checksums.
 * For each checksum item, verifies the first few blocks' CRC32C against
 * the stored checksum values as a sanity check.
 *
 * Returns: 0 on success (even if tree is empty), negative errno on error
 */
static int btrfs_parse_csum_tree(struct btrfs_priv *bp)
{
    uint8_t buf[4096];
    uint8_t verify_buf[4096];
    uint32_t item_idx;
    int exact;
    int ret;
    uint32_t csum_item_count;

    if (bp->nodesize > sizeof(buf)) {
        kprintf("[btrfs] csum tree: nodesize %u exceeds stack buffer\n",
                bp->nodesize);
        return -EINVAL;
    }

    if (bp->csum_root_bytenr == 0) {
        kprintf("[btrfs] csum tree: root not available\n");
        return 0;
    }

    kprintf("[btrfs] csum tree: root=0x%llx level=%u csum_type=%u\n",
            (unsigned long long)bp->csum_root_bytenr,
            (unsigned)bp->csum_root_level,
            (unsigned)bp->csum_type);

    csum_item_count = 0;

    /* Walk the checksum tree by repeated key search */
    {
        uint64_t search_obj = 0;
        uint8_t  search_type = 0;
        uint64_t search_off = 0;

        while (csum_item_count < 200) {
            ret = btrfs_search_tree(bp, bp->csum_root_bytenr,
                                     bp->csum_root_level,
                                     search_obj, search_type, search_off,
                                     buf, sizeof(buf),
                                     &item_idx, &exact);
            if (ret < 0)
                break;

            struct btrfs_header *hdr = (struct btrfs_header *)buf;
            if (item_idx >= hdr->nritems)
                break;

            struct btrfs_item *items = (struct btrfs_item *)
                (buf + sizeof(struct btrfs_header));

            uint64_t cur_obj = items[item_idx].key.objectid;
            uint8_t  cur_typ = items[item_idx].key.type;
            uint64_t cur_off = items[item_idx].key.offset;

            if (cur_typ == BTRFS_CSUM_ITEM_KEY) {
                uint32_t off = items[item_idx].offset;
                uint32_t sz  = items[item_idx].size;
                uint32_t num_csums = sz / 4;  /* 4 bytes per CRC32C */
                uint32_t block_size = bp->sectorsize ? bp->sectorsize : 4096;
                uint64_t block_start = cur_obj * block_size;

                kprintf("[btrfs]   csum item: logical=0x%llx count=%u "
                        "num_csums=%u\n",
                        (unsigned long long)cur_obj,
                        (unsigned)cur_off, num_csums);

                /* Verify the first checksum in this item if possible */
                if (num_csums > 0 && sz >= 4 &&
                    off <= bp->nodesize && 4 <= bp->nodesize - off &&
                    block_start + block_size <= (uint64_t)bp->nodesize * 1024) {
                    /* Read the stored checksum from the item data */
                    uint32_t stored_csum = le32(buf + off);

                    /* Try to read the corresponding data block from disk
                     * and verify its CRC32C.  Non-fatal if read fails. */
                    uint64_t physical_addr;
                    if (btrfs_chunk_map(bp, block_start,
                                         &physical_addr) == 0) {
                        uint64_t lba = physical_addr / 512;
                        for (uint32_t k = 0; k < block_size / 512; k++) {
                            if (blockdev_read_sectors(
                                    bp->dev_id, (uint32_t)(lba + k), 1,
                                    verify_buf + k * 512) != 0) {
                                break;
                            }
                        }
                        uint32_t actual_csum = crc32c(0, verify_buf,
                                                       block_size);
                        if (actual_csum != stored_csum) {
                            kprintf("[btrfs]     block 0x%llx csum "
                                    "MISMATCH: stored=0x%08x "
                                    "actual=0x%08x\n",
                                    (unsigned long long)block_start,
                                    stored_csum, actual_csum);
                        } else {
                            kprintf("[btrfs]     block 0x%llx csum OK\n",
                                    (unsigned long long)block_start);
                        }
                    }
                }

                csum_item_count++;
            }

            /* Advance search past this key */
            search_obj = cur_obj;
            search_type = cur_typ;
            search_off = cur_off + 1;
            if (search_off == 0) {
                search_type++;
                if (search_type == 0)
                    search_obj++;
            }
        }
    }

    kprintf("[btrfs] csum tree walk: %u checksum items\n",
            csum_item_count);
    return 0;
}

/* ── Find fs root (convenience wrapper) ────────────────────────── */

static int btrfs_find_fs_root(struct btrfs_priv *bp)
{
    return btrfs_parse_root_tree(bp);
}

/* ── Path resolution ───────────────────────────────────────────── */

/* Forward declaration — btrfs_lookup is defined below this block */
static uint64_t btrfs_lookup(struct btrfs_priv *bp, uint64_t dir_id,
                              const char *name, int namelen);

/**
 * btrfs_resolve_dirid - Walk a VFS path and resolve to a Btrfs dir_id
 * @bp: Btrfs private data
 * @path: Absolute VFS path (e.g. "/", "/subdir")
 * @dir_id_out: On success, filled with the resolved directory objectid
 *
 * Starts at the fs tree root dirid and walks each path component
 * through btrfs_lookup.  Returns 0 on success, negative errno on
 * failure (typically -ENOENT for a missing component).
 */
static int btrfs_resolve_dirid(struct btrfs_priv *bp, const char *path,
                                uint64_t *dir_id_out)
{
	uint64_t dir_id = bp->fs_root_dirid;

	if (!path || path[0] != '/')
		return -EINVAL;

	/* Root path — no component walking needed */
	if (path[1] == '\0') {
		*dir_id_out = dir_id;
		return 0;
	}

	/* Walk each path component */
	const char *p = path + 1;
	while (*p) {
		const char *slash = strchr(p, '/');
		int clen = slash ? (int)(slash - p) : (int)strlen(p);

		if (clen <= 0 || clen >= 256)
			return -ENOENT;

		uint64_t next = btrfs_lookup(bp, dir_id, p, clen);
		if (next == 0)
			return -ENOENT;

		dir_id = next;
		if (!slash)
			break;
		p = slash + 1;
	}

	*dir_id_out = dir_id;
	return 0;
}

/* Look up a directory entry by name in a given parent directory inode.
 * Returns objectid of the found entry, or 0 if not found. */
static uint64_t btrfs_lookup(struct btrfs_priv *bp, uint64_t dir_id,
                              const char *name, int namelen)
{
    uint8_t buf[4096];
    uint32_t item_idx;
    int exact;

    /* Search for DIR_ITEM with key (dir_id, DIR_ITEM_KEY, name_hash) */
    /* Btrfs uses CRC32C hash of the name for dir_item offset.
     * For simplicity, we'll iterate all DIR_ITEM keys in the directory. */
    /* We search for the first DIR_ITEM key >= (dir_id, DIR_ITEM_KEY, 0) */
    if (btrfs_search_tree(bp, bp->fs_root_bytenr, bp->fs_root_level,
                           dir_id, BTRFS_DIR_ITEM_KEY, 0,
                           buf, sizeof(buf), &item_idx, &exact) < 0)
        return 0;

    /* Iterate all items in the current leaf and subsequent leaves */
    /* For simplicity, iterate items on the current leaf */
    struct btrfs_header *hdr = (struct btrfs_header *)buf;
    struct btrfs_item *items = (struct btrfs_item *)(buf + sizeof(struct btrfs_header));

    for (uint32_t i = item_idx; i < hdr->nritems; i++) {
        if (items[i].key.objectid != dir_id)
            break;
        if (items[i].key.type != BTRFS_DIR_ITEM_KEY &&
            items[i].key.type != BTRFS_DIR_INDEX_KEY)
            continue;

        uint32_t off = items[i].offset;
        uint32_t sz = items[i].size;
        if (off > bp->nodesize || sz > bp->nodesize - off)
            continue;

        /* Parse dir_item entries (may be multiple in one item) */
        uint32_t consumed = 0;
        while (consumed + sizeof(struct btrfs_dir_item) <= sz) {
            struct btrfs_dir_item *di = (struct btrfs_dir_item *)(buf + off + consumed);
            uint16_t name_len = di->name_len;
            uint16_t data_len = di->data_len;

            /* Check if name matches */
            if (name_len == (uint16_t)namelen &&
                (consumed + sizeof(struct btrfs_dir_item) + name_len <= sz)) {
                char *dname = (char *)(buf + off + consumed + sizeof(struct btrfs_dir_item));
                if (memcmp(dname, name, namelen) == 0) {
                    return di->location.objectid;
                }
            }

            consumed += (uint32_t)(sizeof(struct btrfs_dir_item) + name_len);
            /* Align to 8 bytes */
            consumed = (consumed + 7) & ~7;
        }
    }
    return 0;
}

/* ── Inode reading ─────────────────────────────────────────────── */

static int btrfs_read_inode_data(struct btrfs_priv *bp, uint64_t ino,
                                  struct btrfs_inode_item *inode_out,
                                  uint32_t *size_out)
{
    uint8_t buf[4096];
    uint32_t item_idx;
    int exact;

    if (btrfs_search_tree(bp, bp->fs_root_bytenr, bp->fs_root_level,
                           ino, BTRFS_INODE_ITEM_KEY, 0,
                           buf, sizeof(buf), &item_idx, &exact) < 0)
        return -1;

    if (!exact)
        return -1;

    uint8_t item_data[512];
    uint32_t item_size;
    if (btrfs_read_item_data(bp, bp->fs_root_bytenr, buf, item_idx,
                              item_data, &item_size) < 0)
        return -1;

    if (item_size < sizeof(struct btrfs_inode_item))
        return -1;

    memcpy(inode_out, item_data, sizeof(struct btrfs_inode_item));
    *size_out = (uint32_t)inode_out->size;
    return 0;
}

/* ── Subvolume name lookup ─────────────────────────────────────── */

/**
 * btrfs_get_subvolume_name - Look up a subvolume's name via ROOT_REF
 * @bp: Btrfs private data
 * @subvol_id: Subvolume objectid to look up
 * @name_buf: Output buffer for the name
 * @buf_size: Size of name_buf
 *
 * Searches the root tree for a ROOT_REF_KEY item matching the given
 * subvolume objectid.  ROOT_REF entries in Btrfs record the name of
 * a subvolume as a variable-length string following the fixed-size
 * btrfs_root_ref header.
 *
 * Returns: The length of the name on success, or 0 if no name was
 *          found (e.g. the subvolume is the default FS_TREE which
 *          has no ROOT_REF entry, or the item was not present).
 */
static int btrfs_get_subvolume_name(struct btrfs_priv *bp,
                                     uint64_t subvol_id,
                                     char *name_buf, int buf_size)
{
    uint8_t buf[4096];
    uint32_t item_idx;
    int exact;
    int ret;

    if (!bp || !name_buf || buf_size <= 0)
        return 0;

    /* Search for the first ROOT_REF_KEY item >= (subvol_id, ROOT_REF_KEY, 0) */
    ret = btrfs_search_tree(bp, bp->root_bytenr, bp->root_level,
                             subvol_id, BTRFS_ROOT_REF_KEY, 0,
                             buf, sizeof(buf), &item_idx, &exact);
    if (ret < 0)
        return 0;

    struct btrfs_header *hdr = (struct btrfs_header *)buf;
    if (item_idx >= hdr->nritems)
        return 0;

    struct btrfs_item *items = (struct btrfs_item *)
        (buf + sizeof(struct btrfs_header));

    uint64_t cur_obj = items[item_idx].key.objectid;
    uint8_t  cur_typ = items[item_idx].key.type;

    /* Check that the found item belongs to our subvolume and is a ROOT_REF */
    if (cur_obj != subvol_id || cur_typ != BTRFS_ROOT_REF_KEY)
        return 0;

    uint32_t off = items[item_idx].offset;
    uint32_t sz  = items[item_idx].size;

    if (off > bp->nodesize || sz > bp->nodesize - off ||
        sz < sizeof(struct btrfs_root_ref))
        return 0;

    struct btrfs_root_ref *rr = (struct btrfs_root_ref *)(buf + off);
    uint16_t name_len = rr->name_len;

    if (name_len > 0 && name_len < (uint16_t)buf_size &&
        (uint32_t)(sizeof(struct btrfs_root_ref) + name_len) <= sz) {
        memcpy(name_buf, buf + off + sizeof(struct btrfs_root_ref),
               name_len);
        name_buf[name_len] = '\0';
        return (int)name_len;
    }

    return 0;
}

/* ── Subvolume listing ─────────────────────────────────────────── */

/**
 * btrfs_list_subvolumes - Walk the root tree and log all subvolumes
 * @bp: Btrfs private data (root_bytenr/level must be populated)
 *
 * Iterates all ROOT_ITEM_KEY entries in the root tree in key order
 * by advancing the search objectid.  For each subvolume whose
 * objectid >= BTRFS_FIRST_FREE_OBJECTID (256), reads the ROOT_ITEM
 * and logs its objectid, UUID prefix, root_dirid, generation, and
 * root_refs.  Also attempts to resolve the subvolume name from
 * ROOT_REF items.
 *
 * The FS_TREE (objectid 5) is also listed as the default subvolume.
 *
 * Returns: The number of subvolumes listed, or negative errno on
 *          failure.
 */
static int btrfs_list_subvolumes(struct btrfs_priv *bp)
{
    uint8_t buf[4096];
    uint32_t item_idx;
    int exact;
    int ret;
    int count;
    uint64_t search_obj;
    uint8_t  search_type;
    uint64_t search_off;

    if (!bp)
        return -EINVAL;

    kprintf("[btrfs] Subvolume listing:\n");

    count = 0;
    search_obj = 0;
    search_type = BTRFS_ROOT_ITEM_KEY;
    search_off = 0;

    while (count < 256) {
        ret = btrfs_search_tree(bp, bp->root_bytenr, bp->root_level,
                                 search_obj, search_type, search_off,
                                 buf, sizeof(buf),
                                 &item_idx, &exact);
        if (ret < 0)
            break;

        struct btrfs_header *hdr = (struct btrfs_header *)buf;
        if (item_idx >= hdr->nritems)
            break;

        struct btrfs_item *items = (struct btrfs_item *)
            (buf + sizeof(struct btrfs_header));

        uint64_t cur_obj = items[item_idx].key.objectid;
        uint8_t  cur_typ = items[item_idx].key.type;
        uint64_t cur_off = items[item_idx].key.offset;

        /* Past all ROOT_ITEM_KEY items in the tree */
        if (cur_typ != BTRFS_ROOT_ITEM_KEY)
            break;

        uint32_t off = items[item_idx].offset;
        uint32_t sz  = items[item_idx].size;

        if (off <= bp->nodesize && sz <= bp->nodesize - off &&
            sz >= sizeof(struct btrfs_root_item)) {
            struct btrfs_root_item *ri =
                (struct btrfs_root_item *)(buf + off);

            /* List subvolumes (objectid >= 256) and the FS_TREE root */
            if (cur_obj >= BTRFS_FIRST_FREE_OBJECTID ||
                cur_obj == BTRFS_FS_TREE_OBJECTID) {
                char subvol_name[256];
                int name_len;

                name_len = btrfs_get_subvolume_name(bp, cur_obj,
                                                     subvol_name,
                                                     sizeof(subvol_name));

                kprintf("[btrfs]   Subvol %llu",
                        (unsigned long long)cur_obj);
                if (cur_obj == BTRFS_FS_TREE_OBJECTID)
                    kprintf(" (FS_TREE)");
                kprintf(": UUID=%02x%02x%02x%02x... "
                        "dirid=%llu gen=%llu refs=%u",
                        (unsigned)ri->uuid[0],
                        (unsigned)ri->uuid[1],
                        (unsigned)ri->uuid[2],
                        (unsigned)ri->uuid[3],
                        (unsigned long long)ri->root_dirid,
                        (unsigned long long)ri->generation,
                        (unsigned)ri->root_refs);
                if (name_len > 0)
                    kprintf(" name=%s", subvol_name);
                kprintf("\n");
                count++;
            }
        }

        /* Advance search to the next objectid to find more ROOT_ITEM entries.
         * Skip current offset by incrementing it; if offset wraps, increment
         * type; if type wraps, increment objectid. */
        search_off = cur_off + 1;
        if (search_off == 0) {
            search_type = cur_typ + 1;
            if (search_type == 0)
                search_obj = cur_obj + 1;
        } else {
            search_type = cur_typ;
            search_obj = cur_obj;
        }
    }

    kprintf("[btrfs] Subvolume listing: %d subvolume(s) found\n", count);
    return count;
}

/* ── Snapshot detection (read-only) ──────────────────────────────── */

/**
 * btrfs_detect_snapshots - Detect and report Btrfs snapshots (read-only)
 * @bp: Btrfs private data (root_bytenr/level must be populated)
 *
 * Walks the root tree looking for ROOT_ITEM_KEY entries.  For each
 * subvolume, examines the parent_uuid field: a non-zero parent_uuid
 * indicates the subvolume is a snapshot (created via "btrfs subvolume
 * snapshot").  Cross-references UUIDs to identify the parent subvolume
 * by its objectid.
 *
 * The detection performs two passes over the root tree:
 *   Pass 1: Collects all subvolume UUIDs into a local map.
 *   Pass 2: For each subvolume with non-zero parent_uuid, looks up
 *           the parent in the UUID map and logs the snapshot.
 *
 * Returns: The number of snapshots detected, or negative errno on
 *          failure.
 */
static int btrfs_detect_snapshots(struct btrfs_priv *bp)
{
    uint8_t buf[4096];
    uint32_t item_idx;
    int exact;
    int ret;
    int snapshot_count;
    int total_subvols;
    uint64_t search_obj;
    uint8_t  search_type;
    uint64_t search_off;

    /* Map UUID -> subvolume objectid (up to 256 subvolumes) */
    struct {
        uint64_t objectid;
        uint8_t  uuid[16];
    } uuid_map[256];
    int num_uuid;

    if (!bp)
        return -EINVAL;

    kprintf("[btrfs] Snapshot detection:\n");

    num_uuid = 0;

    /* Pass 1: collect all subvolume UUIDs */
    search_obj = 0;
    search_type = BTRFS_ROOT_ITEM_KEY;
    search_off = 0;

    while (num_uuid < 256) {
        ret = btrfs_search_tree(bp, bp->root_bytenr, bp->root_level,
                                 search_obj, search_type, search_off,
                                 buf, sizeof(buf),
                                 &item_idx, &exact);
        if (ret < 0)
            break;

        struct btrfs_header *hdr = (struct btrfs_header *)buf;
        if (item_idx >= hdr->nritems)
            break;

        struct btrfs_item *items = (struct btrfs_item *)
            (buf + sizeof(struct btrfs_header));

        uint64_t cur_obj = items[item_idx].key.objectid;
        uint8_t  cur_typ = items[item_idx].key.type;
        uint64_t cur_off = items[item_idx].key.offset;

        /* Past all ROOT_ITEM_KEY items in the tree */
        if (cur_typ != BTRFS_ROOT_ITEM_KEY)
            break;

        uint32_t off = items[item_idx].offset;
        uint32_t sz  = items[item_idx].size;

        if (off <= bp->nodesize && sz <= bp->nodesize - off &&
            sz >= sizeof(struct btrfs_root_item)) {
            struct btrfs_root_item *ri =
                (struct btrfs_root_item *)(buf + off);

            /* Track subvolumes (objectid >= 256) and the FS_TREE root */
            if (cur_obj >= BTRFS_FIRST_FREE_OBJECTID ||
                cur_obj == BTRFS_FS_TREE_OBJECTID) {
                uuid_map[num_uuid].objectid = cur_obj;
                memcpy(uuid_map[num_uuid].uuid, ri->uuid, 16);
                num_uuid++;
            }
        }

        /* Advance search past this key to find next ROOT_ITEM_KEY */
        search_off = cur_off + 1;
        if (search_off == 0) {
            search_type = cur_typ + 1;
            if (search_type == 0)
                search_obj = cur_obj + 1;
        } else {
            search_type = cur_typ;
            search_obj = cur_obj;
        }
    }

    kprintf("[btrfs]   Collected %d subvolume UUID(s)\n", num_uuid);

    /* Pass 2: detect snapshots by checking parent_uuid */
    snapshot_count = 0;
    total_subvols = 0;

    search_obj = 0;
    search_type = BTRFS_ROOT_ITEM_KEY;
    search_off = 0;

    while (total_subvols < 256) {
        uint8_t zero_uuid[16];
        int resolved;
        int i;

        ret = btrfs_search_tree(bp, bp->root_bytenr, bp->root_level,
                                 search_obj, search_type, search_off,
                                 buf, sizeof(buf),
                                 &item_idx, &exact);
        if (ret < 0)
            break;

        struct btrfs_header *hdr = (struct btrfs_header *)buf;
        if (item_idx >= hdr->nritems)
            break;

        struct btrfs_item *items = (struct btrfs_item *)
            (buf + sizeof(struct btrfs_header));

        uint64_t cur_obj = items[item_idx].key.objectid;
        uint8_t  cur_typ = items[item_idx].key.type;
        uint64_t cur_off = items[item_idx].key.offset;

        if (cur_typ != BTRFS_ROOT_ITEM_KEY)
            break;

        uint32_t off = items[item_idx].offset;
        uint32_t sz  = items[item_idx].size;

        if (off <= bp->nodesize && sz <= bp->nodesize - off &&
            sz >= sizeof(struct btrfs_root_item)) {
            struct btrfs_root_item *ri =
                (struct btrfs_root_item *)(buf + off);

            /* Consider only user-visible subvolumes */
            if (cur_obj >= BTRFS_FIRST_FREE_OBJECTID) {
                total_subvols++;

                /* Check if parent_uuid is non-zero -> snapshot */
                memset(zero_uuid, 0, 16);
                if (memcmp(ri->parent_uuid, zero_uuid, 16) != 0) {
                    /* Resolve parent subvolume by UUID cross-reference */
                    resolved = 0;
                    for (i = 0; i < num_uuid; i++) {
                        if (memcmp(uuid_map[i].uuid,
                                   ri->parent_uuid, 16) == 0) {
                            kprintf("[btrfs]   Snapshot %llu "
                                    "(parent=%llu otransid=%llu "
                                    "stransid=%llu)\n",
                                    (unsigned long long)cur_obj,
                                    (unsigned long long)uuid_map[i].objectid,
                                    (unsigned long long)ri->otransid,
                                    (unsigned long long)ri->stransid);
                            resolved = 1;
                            break;
                        }
                    }
                    if (!resolved) {
                        kprintf("[btrfs]   Snapshot %llu "
                                "(parent=unknown otransid=%llu "
                                "stransid=%llu)\n",
                                (unsigned long long)cur_obj,
                                (unsigned long long)ri->otransid,
                                (unsigned long long)ri->stransid);
                    }
                    snapshot_count++;
                }
            }

            /* Also check FS_TREE for snapshot status */
            if (cur_obj == BTRFS_FS_TREE_OBJECTID) {
                memset(zero_uuid, 0, 16);
                if (memcmp(ri->parent_uuid, zero_uuid, 16) != 0) {
                    int resolved2;
                    resolved2 = 0;
                    for (i = 0; i < num_uuid; i++) {
                        if (memcmp(uuid_map[i].uuid,
                                   ri->parent_uuid, 16) == 0) {
                            kprintf("[btrfs]   Snapshot %llu "
                                    "(FS_TREE, parent=%llu)\n",
                                    (unsigned long long)cur_obj,
                                    (unsigned long long)uuid_map[i].objectid);
                            resolved2 = 1;
                            break;
                        }
                    }
                    if (!resolved2) {
                        kprintf("[btrfs]   Snapshot %llu "
                                "(FS_TREE, parent=unknown)\n",
                                (unsigned long long)cur_obj);
                    }
                    snapshot_count++;
                }
            }
        }

        /* Advance search past this key */
        search_off = cur_off + 1;
        if (search_off == 0) {
            search_type = cur_typ + 1;
            if (search_type == 0)
                search_obj = cur_obj + 1;
        } else {
            search_type = cur_typ;
            search_obj = cur_obj;
        }
    }

    kprintf("[btrfs] Snapshot detection: %d snapshot(s) out of %d "
            "subvolume(s) scanned\n",
            snapshot_count, total_subvols);
    return snapshot_count;
}

/* ── VFS operations ────────────────────────────────────────────── */

/**
 * btrfs_read - Read file data through Btrfs extent tree
 * @priv: Btrfs private data
 * @path: VFS path to the file
 * @buf: Output buffer
 * @max_size: Maximum number of bytes to read
 * @out_size: On success, populated with actual bytes read
 *
 * Btrfs stores file data as EXTENT_DATA_KEY items in the FS tree under
 * the inode's objectid. Each such item is keyed by
 *   (inode_objectid, BTRFS_EXTENT_DATA_KEY, file_offset)
 * and its payload is a struct btrfs_file_extent_item.
 *
 * Two extent types are supported:
 *   BTRFS_EXTENT_DATA_INLINE  (0) — data follows the 5-byte header
 *                                    (type/compression/encryption/other_encoding)
 *                                    directly inside the tree leaf.
 *   BTRFS_EXTENT_DATA_REGULAR (1) — data lives at a logical disk address
 *                                    (disk_bytenr) and is read sector by
 *                                    sector after logical→physical translation
 *                                    via the chunk map.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int btrfs_read(void *priv, const char *path,
                       void *buf, uint32_t max_size, uint32_t *out_size)
{
    struct btrfs_priv *bp = (struct btrfs_priv *)priv;
    struct btrfs_inode_item inode;
    uint64_t ino;
    uint32_t inode_size;
    int ret;
    uint8_t sbuf[4096];
    uint64_t file_size;
    uint32_t to_read;
    uint32_t done;

    if (!bp || !path || !buf || !out_size)
        return -EINVAL;

    /* Resolve path to a Btrfs inode */
    ret = btrfs_resolve_dirid(bp, path, &ino);
    if (ret < 0)
        return ret;

    /* Read the inode item to obtain file size */
    ret = btrfs_read_inode_data(bp, ino, &inode, &inode_size);
    if (ret < 0)
        return ret;

    file_size = inode.size;
    to_read = max_size;
    if ((uint64_t)to_read > file_size)
        to_read = (uint32_t)file_size;

    *out_size = 0;
    if (to_read == 0)
        return 0;

    done = 0;

    /* Walk EXTENT_DATA_KEY items via repeated key search */
    {
        uint64_t search_obj = ino;
        uint8_t  search_type = BTRFS_EXTENT_DATA_KEY;
        uint64_t search_off = 0;

        while (done < to_read) {
            uint32_t item_idx;
            int exact;

            ret = btrfs_search_tree(bp, bp->fs_root_bytenr,
                                     bp->fs_root_level,
                                     search_obj, search_type, search_off,
                                     sbuf, sizeof(sbuf),
                                     &item_idx, &exact);
            if (ret < 0)
                break;

            struct btrfs_header *hdr = (struct btrfs_header *)sbuf;
            if (item_idx >= hdr->nritems)
                break;

            struct btrfs_item *items = (struct btrfs_item *)
                (sbuf + sizeof(struct btrfs_header));

            uint64_t cur_obj = items[item_idx].key.objectid;
            uint8_t  cur_typ = items[item_idx].key.type;
            uint64_t extent_off = items[item_idx].key.offset;

            /* Past this inode's extent data items */
            if (cur_obj != ino || cur_typ != BTRFS_EXTENT_DATA_KEY)
                break;

            uint32_t item_off = items[item_idx].offset;
            uint32_t item_sz  = items[item_idx].size;

            if (item_off > bp->nodesize || item_sz > bp->nodesize - item_off) {
                /* Corrupt item — skip */
                search_off = extent_off + 1;
                goto wrap_search;
            }

            struct btrfs_file_extent_item *fe =
                (struct btrfs_file_extent_item *)(sbuf + item_off);

            /* Handle gap / hole between extents */
            if ((uint64_t)done < extent_off)
                done = (uint32_t)extent_off;

            if (done >= to_read)
                break;

            if (fe->type == BTRFS_EXTENT_DATA_INLINE) {
                /*
                 * Inline extent: data starts at byte 5 of the item
                 * (past type + compression + encryption + other_encoding).
                 */
                static const uint32_t inline_hdr = 5;
                uint32_t inline_len =
                    (item_sz > inline_hdr) ? (item_sz - inline_hdr) : 0;
                uint32_t chunk = to_read - done;

                if (chunk > inline_len)
                    chunk = inline_len;

                if (chunk > 0) {
                    memcpy((uint8_t *)buf + done,
                           sbuf + item_off + inline_hdr, chunk);
                    done += chunk;
                }

                /* Inline extent is always the last extent for a file */
                break;

            } else if (fe->type == BTRFS_EXTENT_DATA_REGULAR &&
                       fe->compression == BTRFS_COMPRESS_NONE) {
                /*
                 * Regular (non-inline, non-compressed) extent.
                 * Data is stored at disk_bytenr on the block device.
                 */
                uint64_t disk_bytenr = fe->disk_bytenr;
                uint64_t num_bytes   = fe->num_bytes;

                if (disk_bytenr == 0)
                    goto search_next;  /* hole — no backing store */

                uint64_t ext_done = (uint64_t)done - extent_off;

                if (ext_done < num_bytes) {
                    uint64_t need = (uint64_t)to_read - (uint64_t)done;
                    if (need > num_bytes - ext_done)
                        need = num_bytes - ext_done;

                    /* Translate logical disk address to physical */
                    uint64_t physical;

                    if (btrfs_chunk_map(bp, disk_bytenr, &physical) < 0)
                        physical = disk_bytenr;

                    uint64_t read_addr = physical + ext_done;
                    uint64_t lba = read_addr / 512;
                    uint32_t nsect = (uint32_t)((need + 511) / 512);
                    uint8_t *dest = (uint8_t *)buf + done;

                    for (uint32_t s = 0; s < nsect; s++) {
                        if (blockdev_read_sectors(bp->dev_id, (uint32_t)(lba + s), 1,
                                                   dest + s * 512) != 0) {
                            /* I/O error — return what we have */
                            goto read_done;
                        }
                    }

                    done += (uint32_t)need;
                }
            }

search_next:
            /* Advance search past the current extent's key offset */
            search_off = extent_off + 1;

wrap_search:
            if (search_off == 0) {
                search_type = cur_typ + 1;
                if (search_type == 0)
                    search_obj = cur_obj + 1;
            }
        }
    }

read_done:
    *out_size = done;
    return 0;
}

static int btrfs_write(void *priv, const char *path,
                        const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

/**
 * btrfs_stat - Stat a file/directory by path, reading the inode item
 * @priv: Btrfs private data
 * @path: VFS path to the file or directory
 * @st:   Output stat buffer
 *
 * Resolves the path to a Btrfs inode number via btrfs_resolve_dirid,
 * reads the INODE_ITEM from the FS tree via btrfs_read_inode_data,
 * and converts the Btrfs on-disk inode fields to the VFS stat format.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int btrfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
    struct btrfs_priv *bp = (struct btrfs_priv *)priv;
    struct btrfs_inode_item inode;
    uint32_t inode_size;
    uint64_t ino;
    int ret;

    if (!bp || !path || !st)
        return -EINVAL;

    memset(st, 0, sizeof(*st));

    /* Resolve path to a Btrfs inode number */
    ret = btrfs_resolve_dirid(bp, path, &ino);
    if (ret < 0)
        return ret;

    /* Read the INODE_ITEM from the FS tree */
    ret = btrfs_read_inode_data(bp, ino, &inode, &inode_size);
    if (ret < 0)
        return ret;

    /* Convert Btrfs on-disk mode to VFS type */
    uint32_t btrfs_mode = (uint32_t)inode.mode;
    switch (btrfs_mode & S_IFMT) {
    case S_IFREG:
        st->type = VFS_TYPE_FILE;
        break;
    case S_IFDIR:
        st->type = VFS_TYPE_DIR;
        break;
    case S_IFLNK:
        st->type = VFS_TYPE_LINK;
        break;
    default:
        /* Fallback: treat unknown types as regular files */
        st->type = VFS_TYPE_FILE;
        break;
    }

    /* Populate stat fields from the Btrfs inode item */
    st->size      = inode.size;
    st->uid       = (uint16_t)inode.uid;
    st->gid       = (uint16_t)inode.gid;
    st->mode      = (uint16_t)(btrfs_mode & 0xFFFFU);
    st->nlink     = (uint32_t)inode.nlink;
    st->ino       = (uint32_t)ino;
    st->atime     = (uint32_t)inode.atime;
    st->mtime     = (uint32_t)inode.mtime;
    st->dev_major = 0;
    st->dev_minor = 0;

    return 0;
}

static int btrfs_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int btrfs_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

/**
 * btrfs_readdir_names - Read directory entries into a names array
 * @priv: Btrfs private data
 * @path: VFS path to the directory
 * @names: Array of up to @max name buffers (each 64 bytes)
 * @max: Maximum number of entries to return
 *
 * Walks all B-tree leaf nodes that contain directory items for the
 * given directory, using the repeated-search pattern.  This correctly
 * handles directories whose entries span multiple tree leaves.
 *
 * Returns: The number of entries written (positive), or negative errno.
 */
static int btrfs_readdir_names(void *priv, const char *path,
                                char names[][64], int max)
{
	struct btrfs_priv *bp = (struct btrfs_priv *)priv;
	uint64_t dir_id;
	uint8_t buf[4096];
	uint32_t item_idx;
	int exact;
	int ret;
	int count;

	if (!bp || !path || !names || max <= 0)
		return -EINVAL;

	/* Resolve path to a btrfs dir_id */
	ret = btrfs_resolve_dirid(bp, path, &dir_id);
	if (ret < 0)
		return ret;

	count = 0;

	/* Add implicit "." and ".." entries */
	if (count < max)
		memcpy(names[count++], ".", 2);
	if (count < max)
		memcpy(names[count++], "..", 3);

	/* Iterate DIR_ITEM_KEY (type 84) entries using repeated-search.
	 * In Btrfs, each file has at least one DIR_ITEM_KEY entry whose
	 * offset field is the crc32c hash of the filename.  A single tree
	 * item may contain multiple btrfs_dir_item structs when two names
	 * hash to the same value (a collision slot).
	 *
	 * By advancing search_off past the current item's offset after
	 * each item, we naturally walk across leaf boundaries. */
	{
		uint64_t search_obj = dir_id;
		uint8_t  search_type = BTRFS_DIR_ITEM_KEY;
		uint64_t search_off = 0;

		while (count < max) {
			ret = btrfs_search_tree(bp, bp->fs_root_bytenr,
						bp->fs_root_level,
						search_obj, search_type,
						search_off,
						buf, sizeof(buf),
						&item_idx, &exact);
			if (ret < 0)
				break;

			struct btrfs_header *hdr = (struct btrfs_header *)buf;
			if (item_idx >= hdr->nritems)
				break;

			struct btrfs_item *items = (struct btrfs_item *)
				(buf + sizeof(struct btrfs_header));

			uint64_t cur_obj = items[item_idx].key.objectid;
			uint8_t  cur_typ = items[item_idx].key.type;
			uint64_t cur_off = items[item_idx].key.offset;

			/* Past the end of this directory's items */
			if (cur_obj != dir_id)
				break;

			/* Skip non-directory-item types (e.g. INODE_ITEM,
			 * INODE_REF) that may appear under the same objectid */
			if (cur_typ != BTRFS_DIR_ITEM_KEY &&
			    cur_typ != BTRFS_DIR_INDEX_KEY) {
				search_off = cur_off + 1;
				if (search_off == 0) {
					search_type = cur_typ + 1;
					if (search_type == 0)
						search_obj = cur_obj + 1;
				}
				continue;
			}

			/* Parse each btrfs_dir_item inside this item */
			uint32_t off = items[item_idx].offset;
			uint32_t sz  = items[item_idx].size;
			if (off > bp->nodesize || sz > bp->nodesize - off) {
				search_off = cur_off + 1;
				if (search_off == 0) {
					search_type = cur_typ + 1;
					if (search_type == 0)
						search_obj = cur_obj + 1;
				}
				continue;
			}
			uint32_t consumed = 0;

			while (consumed + sizeof(struct btrfs_dir_item) <= sz &&
			       count < max) {
				struct btrfs_dir_item *di =
					(struct btrfs_dir_item *)(buf + off +
								 consumed);
				uint16_t name_len = di->name_len;
				uint16_t total = sizeof(struct btrfs_dir_item) +
						 name_len;

				if (name_len > 0 &&
				    consumed + total <= sz &&
				    name_len <= 63) {
					memcpy(names[count],
					       buf + off + consumed +
					       sizeof(struct btrfs_dir_item),
					       name_len);
					names[count][name_len] = '\0';
					count++;
				}

				consumed += total;
				/* Btrfs dir_item entries are 8-byte aligned */
				consumed = (consumed + 7) & ~7;
			}

			/* Advance search past this item's key */
			search_off = cur_off + 1;
			if (search_off == 0) {
				search_type = cur_typ + 1;
				if (search_type == 0)
					search_obj = cur_obj + 1;
			}
		}
	}

	return count;
}

static int btrfs_readdir(void *priv, const char *path)
{
	struct btrfs_priv *bp = (struct btrfs_priv *)priv;
	char names[64][64];
	int n;

	if (!bp)
		return -EINVAL;

	n = btrfs_readdir_names(priv, path, names, 64);
	if (n < 0)
		return n;

	for (int i = 0; i < n; i++)
		kprintf("  %s\n", names[i]);

	return 0;
}

static struct vfs_ops btrfs_ops = {
	.read          = btrfs_read,
	.write         = btrfs_write,
	.stat          = btrfs_stat,
	.create        = btrfs_create,
	.unlink        = btrfs_unlink,
	.readdir       = btrfs_readdir,
	.readdir_names = btrfs_readdir_names,
};

/* ── Probe ─────────────────────────────────────────────────────── */

int btrfs_probe(uint8_t dev_id)
{
    uint8_t buf[BTRFS_SB_SIZE];
    uint64_t sb_lba = BTRFS_SUPER_OFFSET / 512;

    /* Check device is at least 64KB */
    uint64_t total_sectors = blockdev_get_sectors(dev_id);
    if (total_sectors * 512 < BTRFS_MIN_DEV_SIZE)
        return -ENODEV;

    /* Read primary superblock copy at offset 0x10000 */
    for (uint32_t i = 0; i < 8; i++) {
        if (blockdev_read_sectors(dev_id, (uint32_t)(sb_lba + i), 1, buf + i * 512) != 0)
            return -EIO;
    }

    /* Check magic at offset 0x40: "_BHRfS_M" */
    static const uint8_t expected_magic[BTRFS_MAGIC_SIZE] = {
        0x5F, 0x42, 0x48, 0x52, 0x66, 0x53, 0x5F, 0x4D
    };
    if (memcmp(buf + 0x40, expected_magic, BTRFS_MAGIC_SIZE) != 0)
        return -EINVAL;

    kprintf("[btrfs] detected on dev %u\n", dev_id);
    return 0;
}

/* ── Init ──────────────────────────────────────────────────────── */

int __init btrfs_init(void)
{
    kprintf("[btrfs] Btrfs read-only filesystem initialized\n");
    vfs_register_filesystem("btrfs", &btrfs_ops);
    return 0;
}

#ifndef MODULE
fs_initcall(btrfs_init);
#endif

#ifdef MODULE
int init_module(void) { return btrfs_init(); }
void __exit cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Btrfs — read-only, single-device, non-raid");
#endif

/* ── btrfs_mount ─────────────────────────────────────── */
static int btrfs_mount(const char *source, const char *target,
                const char *type, unsigned long flags)
{
    (void)type;
    (void)flags;

    /* Determine device ID from source string ("dev0", "dev1", ...) */
    uint8_t dev_id = 0;
    if (source && source[0]) {
        if (strncmp(source, "dev", 3) == 0 && source[3] >= '0' &&
            source[3] <= '9')
            dev_id = (uint8_t)(source[3] - '0');
    }

    /* Allocate and initialize private data */
    struct btrfs_priv *bp = (struct btrfs_priv *)
        kmalloc(sizeof(struct btrfs_priv));
    if (!bp)
        return -ENOMEM;
    memset(bp, 0, sizeof(*bp));
    bp->dev_id = dev_id;

    /* Parse superblock into private data */
    int ret = btrfs_parse_superblock(bp);
    if (ret < 0) {
        kprintf("[btrfs] mount failed: superblock parse error %d\n", ret);
        kfree(bp);
        return ret;
    }

    /* Build chunk tree for logical→physical address translation */
    ret = btrfs_build_chunk_tree(bp);
    if (ret < 0) {
        kprintf("[btrfs] mount failed: chunk tree error %d\n", ret);
        kfree(bp);
        return ret;
    }

    /* Find FS tree root for the default subvolume */
    ret = btrfs_find_fs_root(bp);
    if (ret < 0) {
        kprintf("[btrfs] mount failed: FS root error %d\n", ret);
        kfree(bp);
        return ret;
    }

    /* Parse extent tree (non-fatal — informational walk) */
    btrfs_parse_extent_tree(bp);

    /* Parse checksum tree (non-fatal — informational walk + verification) */
    btrfs_parse_csum_tree(bp);

    /* List subvolumes (non-fatal — informational) */
    btrfs_list_subvolumes(bp);

    /* Detect snapshots (non-fatal — informational) */
    btrfs_detect_snapshots(bp);

    /* Register with VFS */
    ret = vfs_mount(target, &btrfs_ops, bp);
    if (ret < 0) {
        kprintf("[btrfs] vfs_mount failed: %d\n", ret);
        kfree(bp);
        return ret;
    }

    kprintf("[btrfs] mounted %s on %s (nodesize=%u)\n",
            source, target, bp->nodesize);
    return 0;
}
/* ── btrfs_umount ────────────────────────────────────── */
static int btrfs_umount(const char *target)
{
    (void)target;
    kprintf("[btrfs] Btrfs unmounted\n");
    return 0;
}
/* ── btrfs_statfs ────────────────────────────────────── */
static int btrfs_statfs(void *stat)
{
    struct vfs_statfs *st = (struct vfs_statfs *)stat;
    if (st) {
        st->f_bsize = 4096;
        st->f_blocks = 0;
        st->f_bfree = 0;
        st->f_files = 0;
        st->f_ffree = 0;
        st->f_namelen = 255;
    }
    return 0;
}
/* ── btrfs_sync ──────────────────────────────────────── */
static int btrfs_sync(void *file)
{
    (void)file;
    kprintf("[btrfs] Sync complete\n");
    return 0;
}
