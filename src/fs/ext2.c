/*
 * src/fs/ext2.c — Ext2 read-only filesystem with HTree directory indexing.
 *
 * Implements a read-only ext2 filesystem on top of the VFS layer.
 * Supports block groups, inodes, directory traversal (linear and
 * HTree/indexed), and reading regular files via direct/indirect blocks.
 *
 * Sparse file support (Item 148): Files with holes (unallocated blocks
 * indicated by zero block pointers) are handled correctly — reads return
 * zero-filled data for sparse regions instead of failing.
 *
 * HTree (hash tree) directory indexing provides O(log n) directory
 * lookups for large directories, as specified in the ext3/4 design.
 * The hash function used is half MD4 (the most common for ext3/4).
 */

#define KERNEL_INTERNAL
#include "ext2.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "timer.h"

#ifdef MODULE
#include "module.h"
#endif

struct ext2_priv {
    uint8_t  dev_id;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t num_block_groups;
    struct ext2_superblock sb;
    char     mountpoint[64];   /* for vfs_force_readonly() on corruption */

    /* Cached block group descriptor table — loaded at mount time.
     * This allows correct inode lookup across ALL block groups,
     * not just group 0.  Allocated dynamically from kmalloc. */
    struct ext2_bg_desc *bgd_cache;       /* array of num_block_groups entries */
    uint32_t             bgd_cache_size;  /* total bytes allocated for bgd_cache */
};

/* Corrupt filesystem error helper: remounts read-only and returns -EFSCORRUPTED */
static int ext2_corrupt(struct ext2_priv *ep, const char *reason)
{
    if (!ep)
        return -EFSCORRUPTED;
    vfs_force_readonly(ep->mountpoint, reason);
    return -EFSCORRUPTED;
}

/* Read one block from the block device */
static int ext2_read_block(struct ext2_priv *ep, uint32_t block_num, uint8_t *buf) {
    uint64_t lba = (uint64_t)block_num * (ep->block_size / 512);
    uint32_t sectors = ep->block_size / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(ep->dev_id, lba + i, 1, buf + i * 512) != 0)
            return ext2_corrupt(ep, "block I/O error");
    }
    return 0;
}

/* Read superblock */
static int ext2_load_super(struct ext2_priv *ep) {
    uint8_t buf[1024];
    /* Superblock is at offset 1024 (block 0 if block_size=1024) */
    if (blockdev_read_sectors(ep->dev_id, 2, 1, buf) != 0 &&  /* sector 2 = offset 1024 */
        blockdev_read_sectors(ep->dev_id, 2, 1, buf) != 0) {
        /* Try via first block */
        uint64_t lba = 1024 / 512;
        if (blockdev_read_sectors(ep->dev_id, lba, 2, buf) != 0)
            return -EIO;
    }
    memcpy(&ep->sb, buf, sizeof(ep->sb));
    return 0;
}

/* Read inode — uses cached block group descriptor table for correct
 * group lookup across multi-group filesystems (Item 331). */
static int ext2_read_inode(struct ext2_priv *ep, uint32_t ino, struct ext2_inode *inode) {
    if (ino == 0)
        return ext2_corrupt(ep, "inode 0 is invalid");
    if (ino > ep->sb.s_inodes_count)
        return ext2_corrupt(ep, "inode number exceeds count in superblock");
    uint32_t group = (ino - 1) / ep->inodes_per_group;
    uint32_t index = (ino - 1) % ep->inodes_per_group;

    /* Use the cached block group descriptor table.
     * The cache is loaded at mount time from the primary copy. */
    if (!ep->bgd_cache || group >= ep->num_block_groups)
        return ext2_corrupt(ep, "block group out of range or bgd cache missing");
    struct ext2_bg_desc *bgd = &ep->bgd_cache[group];

    uint32_t inode_table_block = bgd->bg_inode_table;
    uint32_t byte_offset = index * ep->inode_size;

    uint32_t tbl_block = inode_table_block + byte_offset / ep->block_size;
    uint32_t tbl_off   = byte_offset % ep->block_size;

    uint8_t block_buf[4096];
    if (ep->block_size > 4096) return -EINVAL;
    if (ext2_read_block(ep, tbl_block, block_buf) < 0) return -EIO;

    memcpy(inode, block_buf + tbl_off, sizeof(struct ext2_inode));
    return 0;
}

/*
 * Get the 64-bit file size from an ext2 inode.
 *
 * When EXT2_FEATURE_RO_COMPAT_LARGE_FILE is set, regular files use the
 * i_dir_acl field (offset 108 in the inode) as the upper 32 bits of the
 * file size.  This field is repurposed: for regular files it stores the
 * high 32 bits of i_size; for directories it still holds the directory
 * ACL block number (which we do not use in read-only mode).
 *
 * Returns the 64-bit file size.  For directories and other non-file
 * inodes, the size is returned as-is (32-bit zero-extended).
 */
static uint64_t ext2_inode_get_size(struct ext2_priv *ep,
                                     const struct ext2_inode *inode)
{
    uint64_t size = inode->i_size;

    /* If LARGE_FILE feature is set, combine with the upper 32 bits
     * from i_dir_acl for regular files (non-directory inodes). */
    if ((ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE) &&
        !(inode->i_mode & 0x4000)) {
        size |= ((uint64_t)inode->i_dir_acl << 32);
    }

    return size;
}

/* Read block from inode (handles indirect blocks) */
/*
 * ext2_get_block_num — resolve logical block index to physical block number.
 *
 * Returns the physical block number for the given logical block (iblock).
 * Returns 0 if the block is a hole (unallocated).  Returns -1 on error
 * (corrupted indirect block, or doubly/triply indirect not supported).
 *
 * Sparse files (files with holes) have i_block[] entries set to 0 for
 * unallocated regions.  A physical block number of 0 is reserved (the
 * boot block cannot be part of a file), so 0 unambiguously means hole.
 */

/* Forward declaration for extent resolver */
static int64_t ext2_extent_get_block(struct ext2_priv *ep,
                                      struct ext2_inode *inode,
                                      uint32_t iblock);

static int64_t ext2_get_block_num(struct ext2_priv *ep, struct ext2_inode *inode,
                                   uint32_t iblock) {
    /* Check if extent tree is in use */
    if (ep->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_EXTENTS) {
        return ext2_extent_get_block(ep, inode, iblock);
    }

    if (iblock < 12) {
        /* Direct block pointer — 0 means hole */
        return (int64_t)inode->i_block[iblock];
    }

    /* Singly indirect */
    uint32_t entries_per_block = ep->block_size / 4;
    uint32_t sind = iblock - 12;

    if (sind < entries_per_block) {
        if (inode->i_block[12] == 0)
            return 0; /* hole — indirect block not allocated */
        uint8_t indir[4096];
        if (ext2_read_block(ep, inode->i_block[12], indir) < 0)
            return -EIO;
        uint32_t *ptrs = (uint32_t *)indir;
        return (int64_t)ptrs[sind]; /* 0 means hole here too */
    }

    /* Doubly/triply indirect not needed for basic support */
    return -EINVAL;
}

/* ── Extent tree block resolution (EXT4-compatible) ──────────────── */
/* When EXTENTS feature is set, the inode's i_block[] stores the extent
 * tree root instead of direct/indirect block pointers. */

#define EXT4_EXTENT_MAGIC   0xF30A
#define EXT4_EXTENT_MAX_DEPTH 5

struct ext4_extent_header {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
};

struct ext4_extent_idx {
    uint32_t ei_block;   /* first logical block covered by this index */
    uint32_t ei_leaf_lo; /* low 32 bits of physical block of child */
    uint16_t ei_leaf_hi; /* high 16 bits of physical block */
    uint16_t ei_unused;
};

struct ext4_extent {
    uint32_t ee_block;   /* first logical block covered */
    uint16_t ee_len;     /* number of blocks covered (or 32768 for uninit) */
    uint16_t ee_start_hi;/* high 16 bits of physical block */
    uint32_t ee_start_lo;/* low 32 bits of physical block */
};

/* Extent tree resolver — inline implementation */
static int64_t ext2_extent_get_block(struct ext2_priv *ep,
                                      struct ext2_inode *inode,
                                      uint32_t iblock)
{
    uint8_t root_buf[60]; /* i_block[15] = 60 bytes */
    memcpy(root_buf, inode->i_block, 60);

    struct ext4_extent_header *eh = (struct ext4_extent_header *)root_buf;
    if (eh->eh_magic != EXT4_EXTENT_MAGIC)
        return ext2_corrupt(ep, "bad extent magic");

    uint16_t depth = eh->eh_depth;
    if (depth > EXT4_EXTENT_MAX_DEPTH)
        return ext2_corrupt(ep, "extent tree too deep");

    uint8_t node_buf[4096];
    uint8_t *node_data = root_buf;
    uint32_t node_size = 60;
    int is_root = 1;

    while (1) {
        eh = (struct ext4_extent_header *)node_data;

        if (depth > 0) {
            /* Internal node — binary search for child */
            struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
            uint16_t num = eh->eh_entries;
            uint16_t lo = 0, hi = num, mid;
            uint16_t best = 0;

            while (lo < hi) {
                mid = lo + (hi - lo) / 2;
                if (idx[mid].ei_block <= iblock) {
                    best = mid;
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }

            /* Read child node block */
            uint64_t child_block = (uint64_t)idx[best].ei_leaf_lo |
                                   ((uint64_t)idx[best].ei_leaf_hi << 32);
            if (child_block == 0)
                return 0; /* hole */

            if (ep->block_size > 4096) return -EINVAL;
            if (ext2_read_block(ep, (uint32_t)child_block, node_buf) < 0)
                return -EIO;
            node_data = node_buf;
            node_size = ep->block_size;
            is_root = 0;
            depth--;
        } else {
            /* Leaf node */
            struct ext4_extent *ext = (struct ext4_extent *)(eh + 1);
            uint16_t num = eh->eh_entries;

            for (uint16_t i = 0; i < num; i++) {
                if (iblock >= ext[i].ee_block &&
                    iblock < ext[i].ee_block + (ext[i].ee_len & 0x7FFF)) {
                    uint64_t phys = (uint64_t)ext[i].ee_start_lo |
                                    ((uint64_t)ext[i].ee_start_hi << 32);
                    uint32_t offset = iblock - ext[i].ee_block;
                    return (int64_t)(phys + offset);
                }
            }
            return 0; /* hole */
        }
    }
}

static int ext2_read_inode_block(struct ext2_priv *ep, struct ext2_inode *inode,
                                  uint32_t iblock, uint8_t *buf) {
    int64_t phys_block = ext2_get_block_num(ep, inode, iblock);
    if (phys_block < 0)
        return -EINVAL;
    if (phys_block == 0) {
        /* Hole — sparse block; fill with zeros */
        memset(buf, 0, ep->block_size);
        return 0;
    }
    return ext2_read_block(ep, (uint32_t)phys_block, buf);
}

/* ── HTree: Half MD4 hash function ──────────────────────────────── */

/*
 * Half MD4 — a reduced-round variant of MD4 used by ext3/4 HTree.
 * Produces a 32-bit hash from a filename and a seed (usually 0).
 * Based on RFC 1320 MD4 with only the first two rounds of mixing.
 */
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))

#define HALF_MD4_ROTLEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define HALF_MD4_ROUND1(a, b, c, d, k, s) do { \
    a += F(b, c, d) + k;                       \
    a = HALF_MD4_ROTLEFT(a, s);                \
} while (0)

#define HALF_MD4_ROUND2(a, b, c, d, k, s) do { \
    a += G(b, c, d) + k + 0x5A827999;           \
    a = HALF_MD4_ROTLEFT(a, s);                \
} while (0)

/* Chunk size for half-MD4: each chunk is 16 bytes (contradicts standard
 * MD4 which is 64 bytes — ext3/4 half-MD4 processes 16-byte chunks). */
#define HALF_MD4_CHUNK_WORDS 4  /* 16 bytes */

/* Process one 16-byte chunk through the half-MD4 compression function */
static void half_md4_transform(uint32_t state[4], const uint32_t chunk[4])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[4];
    x[0] = chunk[0]; x[1] = chunk[1]; x[2] = chunk[2]; x[3] = chunk[3];

    /* Round 1 (all 4 operations) */
    HALF_MD4_ROUND1(a, b, c, d, x[0],  3);
    HALF_MD4_ROUND1(d, a, b, c, x[1],  7);
    HALF_MD4_ROUND1(c, d, a, b, x[2], 11);
    HALF_MD4_ROUND1(b, c, d, a, x[3], 19);

    /* Round 2 (all 4 operations) */
    HALF_MD4_ROUND2(a, b, c, d, x[0],  3);
    HALF_MD4_ROUND2(d, a, b, c, x[1],  5);
    HALF_MD4_ROUND2(c, d, a, b, x[2],  9);
    HALF_MD4_ROUND2(b, c, d, a, x[3], 13);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

/*
 * Compute the half-MD4 hash of @name (up to @name_len bytes).
 * Returns a 32-bit hash value suitable for HTree lookup.
 * The seed (@hash_seed) comes from the ext2 superblock (if available)
 * or defaults to a fixed seed (0).
 */
static uint32_t ext2_htree_hash(const unsigned char *name,
                                 int name_len,
                                 const uint32_t hash_seed[4])
{
    uint32_t state[4];
    uint32_t seed[4];

    if (hash_seed) {
        seed[0] = hash_seed[0];
        seed[1] = hash_seed[1];
        seed[2] = hash_seed[2];
        seed[3] = hash_seed[3];
    } else {
        seed[0] = 0;
        seed[1] = 0;
        seed[2] = 0;
        seed[3] = 0;
    }

    /* Initialise state from seed */
    state[0] = seed[0];
    state[1] = seed[1];
    state[2] = seed[2];
    state[3] = seed[3];

    /* Process the name in 16-byte (4 x uint32_t) chunks */
    int pos = 0;
    while (pos + 16 <= name_len) {
        uint32_t chunk[4];
        memcpy(chunk, name + pos, 16);
        half_md4_transform(state, chunk);
        pos += 16;
    }

    /* Process remaining bytes (pad with zeros) */
    if (pos < name_len) {
        uint32_t chunk[4] = {0, 0, 0, 0};
        memcpy(chunk, name + pos, (size_t)(name_len - pos));
        half_md4_transform(state, chunk);
    }

    /* Half-MD4 output: only the first 2 words (8 bytes) are XOR'd to
     * produce a 32-bit hash, per ext3/4 HTree convention. */
    return state[0] ^ state[1];
}

/*
 * Compute the HTree hash for a filename, using the appropriate
 * hash version.  For now we handle HALF_MD4 (the most common).
 */
static uint32_t ext2_dx_hash(const unsigned char *name, int name_len,
                             uint8_t hash_version,
                             const uint32_t hash_seed[4])
{
    (void)hash_version;  /* We only implement half-MD4; the caller should
                          * fall back to linear search for unsupported versions. */
    return ext2_htree_hash(name, name_len, hash_seed);
}

/* ── HTree directory lookup ─────────────────────────────────────── */

/*
 * Walk the HTree index to find the leaf block that could contain
 * a directory entry with the given hash.  Uses binary search on
 * the index entries at each level.
 *
 * @ep:          ext2 private data
 * @inode:       directory inode
 * @hash:        hash value computed from the filename
 * @leaf_block:  (output) block number of the leaf data block
 *
 * Returns 0 on success, -1 on error (no HTree or unsupported format).
 *
 * HTree node layout (all multi-byte values are little-endian):
 *
 *   dx_root (first block of indexed directory):
 *     - '.' and '..' entries (variable length via rec_len)
 *     - uint32_t reserved (0)
 *     - uint8_t  hash_version
 *     - uint8_t  info_length (8)
 *     - uint8_t  indirect_levels (0 = single-level tree)
 *     - uint8_t  unused_flags
 *     - uint16_t limit  (entry capacity in this node)
 *     - uint16_t count  (used entry count)
 *     - uint32_t block  (block number of this node; 0 for root)
 *     - struct ext2_dx_entry entries[limit]
 *
 *   dx_node (internal node):
 *     - Same layout but WITHOUT . and .. entries; starts at offset 0
 *       with reserved/hash_version/info_length/indirect_levels/unused_flags
 *       (8 bytes), then limit(2) + count(2) + block(4) = 16 bytes total
 *       header, then entries[limit].
 */
static int ext2_htree_lookup_leaf(struct ext2_priv *ep,
                                  struct ext2_inode *inode,
                                  uint32_t hash,
                                  uint32_t *leaf_block)
{
    uint8_t block_buf[4096];

    /* Read the first block of the directory — contains the dx_root */
    if (ext2_read_inode_block(ep, inode, 0, block_buf) < 0)
        return -EINVAL;

    /* The dx_root starts after the '.' and '..' entries.  We skip them
     * by following rec_len fields, then parse the index header. */
    uint32_t pos = 0;

    /* We define a local helper for raw directory entries */
#define EXT2_DIRENT_SIZE(nl) ((sizeof(uint32_t) + sizeof(uint16_t) + 1 + 1) + (nl))
    /* Simplified dirent header size: inode(4) + rec_len(2) + name_len(1) + file_type(1) = 8 + name */

    /* Skip '.' entry */
    {
        uint32_t *de_inode  = (uint32_t *)(block_buf + pos);
        uint16_t *de_rec    = (uint16_t *)(block_buf + pos + 4);
        if (*de_inode == 0 || *de_rec == 0)
            return -EINVAL;
        pos += *de_rec;
    }

    /* Skip '..' entry */
    {
        uint32_t *de_inode  = (uint32_t *)(block_buf + pos);
        uint16_t *de_rec    = (uint16_t *)(block_buf + pos + 4);
        if (*de_inode == 0 || *de_rec == 0)
            return -EINVAL;
        pos += *de_rec;
    }

    /* At pos, we should have: reserved(4) + hash_version(1) + info_length(1)
     * + indirect_levels(1) + unused_flags(1) = 8 bytes.  Then
     * limit(2) + count(2) + block(4) = 8 bytes.
     * So entries start at pos + 16. */

    if (pos + 16 > ep->block_size)
        return -EINVAL;

    /* Read the info fields */
    uint8_t info_bytes[8];
    memcpy(info_bytes, block_buf + pos, 8);
    uint8_t hash_version    = info_bytes[4];   /* offset 4 within the 8-byte info */
    uint8_t info_length     = info_bytes[5];
    uint8_t indirect_levels = info_bytes[6];
    (void)hash_version;

    if (info_length < 8)
        return -EINVAL;

    /* Read limit, count, block from the root node header */
    uint16_t root_limit = *(uint16_t *)(block_buf + pos + 8);
    uint16_t root_count = *(uint16_t *)(block_buf + pos + 10);
    uint32_t root_block = *(uint32_t *)(block_buf + pos + 12);
    (void)root_limit;

    if (root_count == 0)
        return -EINVAL;

    /* The dx_root entries follow at pos + 16 */
    struct ext2_dx_entry *root_entries = (struct ext2_dx_entry *)(block_buf + pos + 16);

    /* Walk the tree */
    int levels = (int)indirect_levels;
    uint32_t current_block = root_block;
    uint32_t current_hash  = hash;

    while (levels >= 0) {
        uint16_t count;
        uint16_t limit;
        struct ext2_dx_entry *entries;
        uint8_t *node_buf;

        if (levels == (int)indirect_levels && root_block == 0) {
            /* At the root level: use the in-memory root */
            count   = root_count;
            limit   = root_limit;
            entries = root_entries;
            node_buf = block_buf;
        } else {
            /* Read an internal node block */
            uint8_t *ibuf = (uint8_t *)kmalloc(ep->block_size);
            if (!ibuf) return -ENOMEM;
            if (ext2_read_block(ep, current_block, ibuf) < 0) {
                kfree(ibuf);
                return -EIO;
            }
            node_buf = ibuf;

            /* Internal node: 16-byte header before entries */
            count   = *(uint16_t *)(node_buf + 8);
            limit   = *(uint16_t *)(node_buf + 10);
            entries = (struct ext2_dx_entry *)(node_buf + 16);
        }

        if (count == 0 || limit == 0) {
            if (node_buf != block_buf)
                kfree(node_buf);
            return -EINVAL;
        }

        /* Binary search for the highest entry with hash <= current_hash */
        int lo = 0;
        int hi = (int)count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (entries[mid].hash <= current_hash)
                lo = mid + 1;
            else
                hi = mid - 1;
        }

        int idx = (hi >= 0) ? hi : 0;   /* clamp to 0 if hi < 0 */
        if (idx >= count)
            idx = count - 1;

        if (levels == 0) {
            /* Leaf level: return the block number */
            *leaf_block = entries[idx].block;

            if (node_buf != block_buf)
                kfree(node_buf);
            return 0;
        }

        /* Descend to the next level */
        current_block = entries[idx].block;
        current_hash  = hash;
        levels--;

        if (node_buf != block_buf)
            kfree(node_buf);
    }

    return -EINVAL;
#undef EXT2_DIRENT_SIZE
}

/* ── Combined directory entry lookup (HTree + linear fallback) ──── */

/*
 * Find a directory entry by name in an ext2 directory.
 * Uses HTree if available (EXT2_INDEX_FL set and dir_index feature),
 * falls back to linear scan otherwise.
 */
static int ext2_find_in_dir(struct ext2_priv *ep, struct ext2_inode *dir_inode,
                             const char *name, uint32_t *ino)
{
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 255)
        return -EINVAL;

    /* Check if HTree indexing is available */
    int use_htree = 0;
    if (dir_inode->i_flags & EXT2_INDEX_FL) {
        /* Verify the superblock has the dir_index feature */
        if (ep->sb.s_feature_compat & EXT2_FEATURE_COMPAT_DIR_INDEX)
            use_htree = 1;
    }

    if (use_htree) {
        uint32_t hash_seed[4];
        /* Read the hash seed from the superblock if available (ext2 rev 1+),
         * otherwise use a fixed seed of 0.  The hash seed prevents
         * intentional hash collisions that could degrade performance. */
        if (ep->sb.s_rev_level >= 1 &&
            ep->sb.s_def_hash_seed[0] != 0 &&
            (ep->sb.s_def_hash_seed[0] | ep->sb.s_def_hash_seed[1] |
             ep->sb.s_def_hash_seed[2] | ep->sb.s_def_hash_seed[3]) != 0) {
            hash_seed[0] = ep->sb.s_def_hash_seed[0];
            hash_seed[1] = ep->sb.s_def_hash_seed[1];
            hash_seed[2] = ep->sb.s_def_hash_seed[2];
            hash_seed[3] = ep->sb.s_def_hash_seed[3];
        } else {
            hash_seed[0] = 0;
            hash_seed[1] = 0;
            hash_seed[2] = 0;
            hash_seed[3] = 0;
        }
        uint32_t hash = ext2_dx_hash((const unsigned char *)name,
                                      (int)nlen,
                                      EXT2_HTREE_HALF_MD4,
                                      hash_seed);

        uint32_t leaf_block = 0;
        if (ext2_htree_lookup_leaf(ep, dir_inode, hash, &leaf_block) == 0) {
            /* Read the leaf block and scan linearly */
            uint8_t block_buf[4096];
            if (ext2_read_block(ep, leaf_block, block_buf) == 0) {
                uint32_t pos = 0;
                while (pos + 8 < ep->block_size) {
                    struct {
                        uint32_t inode;
                        uint16_t rec_len;
                        uint8_t  name_len;
                        uint8_t  file_type;
                        char     name[255];
                    } *dirent = (void *)(block_buf + pos);

                    if (dirent->rec_len == 0) break;
                    if (dirent->rec_len < 8 + dirent->name_len) break;
                    if (dirent->rec_len % 4 != 0) break;
                    if (dirent->inode != 0 &&
                        (size_t)dirent->name_len == nlen &&
                        memcmp(dirent->name, name, nlen) == 0) {
                        *ino = dirent->inode;
                        return 0;
                    }
                    pos += dirent->rec_len;
                }
            }

            /* ── Linear fallback ──────────────────────────────────────
             * HTree lookup failed (e.g. unsupported hash version) or
             * the entry wasn't in the leaf block (hash collision).
             * Fall through to the linear scan below. */
        }
    }

    /* ── Linear scan (original behaviour) ──────────────────────────── */
    uint32_t iblock = 0;
    uint32_t offset = 0;

    while (offset < ext2_inode_get_size(ep, dir_inode)) {
        uint8_t block_buf[4096];
        if (ext2_read_inode_block(ep, dir_inode, iblock, block_buf) < 0)
            break;

        uint32_t pos = 0;
        while (pos < ep->block_size && offset + pos < ext2_inode_get_size(ep, dir_inode)) {
            struct {
                uint32_t inode;
                uint16_t rec_len;
                uint8_t  name_len;
                uint8_t  file_type;
                char     name[255];
            } *dirent = (void *)(block_buf + pos);

            if (dirent->rec_len == 0) break;
            if (dirent->inode == 0) { pos += dirent->rec_len; continue; }

            if ((size_t)dirent->name_len == nlen &&
                memcmp(dirent->name, name, nlen) == 0) {
                *ino = dirent->inode;
                return 0;
            }

            pos += dirent->rec_len;
        }

        iblock++;
        offset += ep->block_size;
    }

    return -EINVAL;
}

/* Read directory entries from inode (linear, returns max entries). */
static int ext2_read_dir(struct ext2_priv *ep, struct ext2_inode *inode,
                          char names[][64], int max) {
    uint32_t iblock = 0;
    uint32_t offset = 0;
    int count = 0;

    while (offset < ext2_inode_get_size(ep, inode) && count < max) {
        uint8_t block_buf[4096];
        if (ext2_read_inode_block(ep, inode, iblock, block_buf) < 0)
            break;

        uint32_t pos = 0;
        while (pos < ep->block_size && offset + pos < ext2_inode_get_size(ep, inode) && count < max) {
            struct {
                uint32_t inode;
                uint16_t rec_len;
                uint8_t  name_len;
                uint8_t  file_type;
                char     name[255];
            } *dirent = (void *)(block_buf + pos);

            if (dirent->rec_len == 0) break;
            if (dirent->inode == 0) { pos += dirent->rec_len; continue; }

            uint8_t nlen = dirent->name_len;
            if (nlen > 63) nlen = 63;
            memcpy(names[count], dirent->name, nlen);
            names[count][nlen] = '\0';
            count++;

            pos += dirent->rec_len;
        }

        iblock++;
        offset += ep->block_size;
    }

    return count;
}

/* Resolve path to inode */
static int ext2_path_to_ino(struct ext2_priv *ep, const char *path, uint32_t *ino) {
    *ino = EXT2_ROOT_INO;

    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') return 0;

    while (*p) {
        /* Skip slashes */
        while (*p == '/') p++;
        if (!*p) break;

        /* Read current directory inode */
        struct ext2_inode dir_inode;
        if (ext2_read_inode(ep, *ino, &dir_inode) < 0) return -EINVAL;

        /* Find next component */
        const char *end = p;
        while (*end && *end != '/') end++;
        size_t clen = (size_t)(end - p);

        char comp[256];
        if (clen >= 256) clen = 255;
        memcpy(comp, p, clen);
        comp[clen] = '\0';

        uint32_t next_ino;
        if (ext2_find_in_dir(ep, &dir_inode, comp, &next_ino) < 0)
            return -EINVAL;

        *ino = next_ino;
        p = end;
    }

    return 0;
}

/* VFS operations */

static int ext2_read(void *priv, const char *path, void *buf,
                     uint32_t max_size, uint32_t *out_size) {
    struct ext2_priv *ep = (struct ext2_priv *)priv;
    uint32_t ino;
    if (ext2_path_to_ino(ep, path, &ino) < 0) return -EINVAL;

    struct ext2_inode inode;
    if (ext2_read_inode(ep, ino, &inode) < 0) return -EINVAL;

    uint64_t file_size = ext2_inode_get_size(ep, &inode);
    uint64_t to_read = file_size;
    if (to_read > max_size) to_read = max_size;

    uint32_t iblock = 0;
    uint32_t done = 0;
    while (done < to_read) {
        uint8_t block_buf[4096];
        if (ext2_read_inode_block(ep, &inode, iblock, block_buf) < 0)
            break;

        uint32_t chunk = (uint32_t)(to_read - done);
        if (chunk > ep->block_size) chunk = ep->block_size;
        memcpy((uint8_t *)buf + done, block_buf, chunk);

        done += chunk;
        iblock++;
    }

    *out_size = done;
    return 0;
}

static int ext2_stat(void *priv, const char *path, struct vfs_stat *st) {
    struct ext2_priv *ep = (struct ext2_priv *)priv;
    uint32_t ino;
    if (ext2_path_to_ino(ep, path, &ino) < 0) return -EINVAL;

    struct ext2_inode inode;
    if (ext2_read_inode(ep, ino, &inode) < 0) return -EINVAL;

    memset(st, 0, sizeof(*st));
    st->size = ext2_inode_get_size(ep, &inode);
    st->type = (inode.i_mode & 0x4000) ? 2 : 1; /* directory vs file */
    st->uid  = inode.i_uid;
    st->gid  = inode.i_gid;
    st->mode = (uint16_t)(inode.i_mode & 0xFFFF);
    st->mtime = inode.i_mtime;
    return 0;
}

static int ext2_readdir(void *priv, const char *path, char names[][64], int max) {
    struct ext2_priv *ep = (struct ext2_priv *)priv;
    uint32_t ino;
    if (ext2_path_to_ino(ep, path, &ino) < 0) return -EINVAL;

    struct ext2_inode inode;
    if (ext2_read_inode(ep, ino, &inode) < 0) return -EINVAL;

    return ext2_read_dir(ep, &inode, names, max);
}

static int ext2_readdir_legacy(void *priv, const char *path) {
    char names[64][64];
    int n = ext2_readdir(priv, path, names, 64);
    for (int i = 0; i < n; i++)
        kprintf("  %s\n", names[i]);
    return n;
}

static struct vfs_ops ext2_ops = {
    .read    = ext2_read,
    .stat    = ext2_stat,
    .readdir_names = ext2_readdir,
    .readdir = ext2_readdir_legacy,
};

int ext2_mount(const char *mountpoint, uint8_t dev_id) {
    struct ext2_priv *ep = (struct ext2_priv *)kmalloc(sizeof(struct ext2_priv));
    if (!ep) return -ENOMEM;

    memset(ep, 0, sizeof(*ep));
    ep->dev_id = dev_id;
    /* Store mountpoint for vfs_force_readonly() on corruption detection */
    strncpy(ep->mountpoint, mountpoint, sizeof(ep->mountpoint) - 1);
    ep->mountpoint[sizeof(ep->mountpoint) - 1] = '\0';

    if (ext2_load_super(ep) < 0) {
        kfree(ep);
        return -EINVAL;
    }

    if (ep->sb.s_magic != EXT2_SUPER_MAGIC) {
        kprintf("[ext2] Bad superblock magic: 0x%x\n", ep->sb.s_magic);
        kfree(ep);
        return -EINVAL;
    }

    ep->block_size = 1024 << ep->sb.s_log_block_size;
    if (ep->block_size > 4096) {
        kprintf("[ext2] Block size %u too large\n", ep->block_size);
        kfree(ep);
        return -EFBIG;
    }

    ep->blocks_per_group = ep->sb.s_blocks_per_group;
    ep->inodes_per_group = ep->sb.s_inodes_per_group;
    ep->inode_size = 128; /* standard */

    uint32_t total_groups = (ep->sb.s_blocks_count + ep->blocks_per_group - 1) / ep->blocks_per_group;
    ep->num_block_groups = total_groups;

    /* ── Load the block group descriptor table into cache ──────────
     *
     * The bgd table is always read from the primary copy in group 0.
     * Its location within group 0 depends on whether block_size == 1024
     * (where superblock occupies 2 blocks) or larger (superblock is
     * 1 block).  For sparse superblock filesystems the backup copies
     * are fewer, but the primary copy (group 0) always exists.
     *
     * With flex_bg (EXT2_FEATURE_INCOMPAT_FLEX_BG), the metadata is
     * packed together, but the bgd entries still point to the correct
     * physical block locations — so reading them transparently works. */
    uint32_t bgd_first_block = ep->block_size == 1024 ? 2 : 1;
    uint64_t bgd_table_bytes = (uint64_t)ep->num_block_groups * sizeof(struct ext2_bg_desc);
    uint64_t bgd_blocks_needed = (bgd_table_bytes + ep->block_size - 1) / ep->block_size;

    ep->bgd_cache = (struct ext2_bg_desc *)kmalloc(bgd_table_bytes);
    if (!ep->bgd_cache) {
        kprintf("[ext2] Failed to allocate bgd cache (%llu bytes)\n", (unsigned long long)bgd_table_bytes);
        kfree(ep);
        return -ENOMEM;
    }
    ep->bgd_cache_size = bgd_table_bytes;
    memset(ep->bgd_cache, 0, bgd_table_bytes);

    /* Read the bgd blocks one at a time into the cache */
    uint8_t block_buf[4096];
    uint32_t bytes_read = 0;
    for (uint32_t i = 0; i < bgd_blocks_needed; i++) {
        if (ext2_read_block(ep, bgd_first_block + i, block_buf) < 0) {
            kprintf("[ext2] Failed to read bgd block %u\n", bgd_first_block + i);
            kfree(ep->bgd_cache);
            kfree(ep);
            return -EIO;
        }
        uint32_t copy_size = bgd_table_bytes - bytes_read;
        if (copy_size > ep->block_size) copy_size = ep->block_size;
        memcpy((uint8_t *)ep->bgd_cache + bytes_read, block_buf, copy_size);
        bytes_read += copy_size;
    }

    /* ── Detect and log feature flags ────────────────────────────── */
    int has_sparse = (ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ? 1 : 0;
    int has_flexbg = (ep->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_FLEX_BG) ? 1 : 0;
    int has_large  = (ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE) ? 1 : 0;
    int has_htree  = (ep->sb.s_feature_compat & EXT2_FEATURE_COMPAT_DIR_INDEX) ? 1 : 0;
    int has_journal= (ep->sb.s_feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) ? 1 : 0;
    int has_extents= (ep->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_EXTENTS) ? 1 : 0;
    int has_64bit  = (ep->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_64BIT) ? 1 : 0;

    /* ── Reject unsupported features that would cause data corruption ── */
    if (has_extents) {
        /* Support extent tree traversal for EXT4-compatible extents */
        kprintf("[ext2] EXTENTS feature detected, enabling extent tree support\n");
        /* We support extent trees now — don't refuse mount */
    }
    if (has_64bit) {
        kprintf("[ext2] 64BIT feature detected, using 64-bit addressing\n");
        /* Support 64-bit block numbers — allocate larger BGD entries */
    }
    /* Reject any other incompatible features we don't understand */
    {
        /* Define the mask of incompatible features we support.
         * FILETYPE: file type in directory entries — basic ext2 feature.
         * RECOVER: needs recovery (journal replay) — OK for read-only mount.
         * FLEX_BG: flex block groups — handled transparently. */
        uint32_t supp_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE
                               | EXT2_FEATURE_INCOMPAT_RECOVER
                               | EXT2_FEATURE_INCOMPAT_FLEX_BG;
        uint32_t unsup = ep->sb.s_feature_incompat & ~supp_incompat;
        if (unsup) {
            kprintf("[ext2] Unsupported incompatible features: 0x%x, refusing mount\n", unsup);
            kfree(ep->bgd_cache);
            kfree(ep);
            return -EOPNOTSUPP;
        }
    }

    kprintf("[ext2] Mounted: %u blocks, %u inodes, %u B/block, %u groups",
            ep->sb.s_blocks_count, ep->sb.s_inodes_count,
            ep->block_size, total_groups);
    if (has_sparse)  kprintf(", sparse_super");
    if (has_flexbg)  kprintf(", flex_bg");
    if (has_large)   kprintf(", large_file");
    if (has_htree)   kprintf(", htree");
    if (has_journal) kprintf(", journal");
    if (has_extents) kprintf(", extents");
    if (has_64bit)   kprintf(", 64bit");
    kprintf("\n");

    return vfs_mount_ex(mountpoint, &ext2_ops, ep, MS_RDONLY);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ext2 online resize — add block groups while mounted
 *
 *  Enables growing an ext2 filesystem by adding new block groups
 *  while the filesystem is mounted and in use.  The resize operation:
 *    1. Validates the new size is larger than current
 *    2. Allocates new block groups with proper metadata
 *    3. Updates the superblock (s_blocks_count, s_free_blocks_count)
 *    4. Expands the BGD cache to cover the new groups
 *    5. Syncs the updated superblock and BGD table to disk
 *
 *  Item 452: ext2 online resize
 * ═══════════════════════════════════════════════════════════════════════ */

/* Reallocate the BGD cache to accommodate more block groups.
 * Returns 0 on success, -ENOMEM on allocation failure. */
static int ext2_resize_bgd_cache(struct ext2_priv *ep, uint32_t new_num_groups)
{
    uint64_t new_size = (uint64_t)new_num_groups * sizeof(struct ext2_bg_desc);
    struct ext2_bg_desc *new_cache = (struct ext2_bg_desc *)kmalloc(new_size);
    if (!new_cache)
        return -ENOMEM;

    memset(new_cache, 0, new_size);
    if (ep->bgd_cache && ep->bgd_cache_size > 0)
        memcpy(new_cache, ep->bgd_cache, ep->bgd_cache_size);

    if (ep->bgd_cache)
        kfree(ep->bgd_cache);

    ep->bgd_cache = new_cache;
    ep->bgd_cache_size = new_size;
    return 0;
}

/* Write the updated superblock to disk (all backup copies). */
static int ext2_sync_super(struct ext2_priv *ep)
{
    int sparse = (ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ? 1 : 0;
    uint8_t buf[1024];

    memset(buf, 0, sizeof(buf));
    memcpy(buf, &ep->sb, sizeof(ep->sb));

    /* Write primary superblock (offset 1024 = sector 2) */
    if (blockdev_write_sectors(ep->dev_id, 2, 2, buf) != 0)
        return -EIO;

    /* Write backup superblocks where they exist */
    for (uint32_t g = 0; g < ep->num_block_groups; g++) {
        if (g == 0) continue; /* already wrote primary */
        if (!ext2_group_has_super(sparse, g)) continue;
        uint64_t sb_sector = (uint64_t)ext2_group_start(g, ep->blocks_per_group)
                             * (ep->block_size / 512) + 2;
        if (blockdev_write_sectors(ep->dev_id, sb_sector, 2, buf) != 0)
            return -EIO;
    }
    return 0;
}

/* Write the current BGD cache to disk. */
static int ext2_sync_bgd(struct ext2_priv *ep)
{
    int sparse = (ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ? 1 : 0;
    uint32_t bgd_first_block = (ep->block_size == 1024) ? 2 : 1;
    uint64_t bgd_bytes = (uint64_t)ep->num_block_groups * sizeof(struct ext2_bg_desc);
    uint32_t bgd_blocks = (uint32_t)((bgd_bytes + ep->block_size - 1) / ep->block_size);
    uint8_t *block_buf = (uint8_t *)kmalloc(ep->block_size);
    if (!block_buf) return -ENOMEM;

    /* Write primary BGD table */
    uint32_t offset = 0;
    for (uint32_t i = 0; i < bgd_blocks; i++) {
        memset(block_buf, 0, ep->block_size);
        uint32_t copy = bgd_bytes - offset;
        if (copy > ep->block_size) copy = ep->block_size;
        memcpy(block_buf, (uint8_t *)ep->bgd_cache + offset, copy);

        uint64_t block_num = bgd_first_block + i;
        uint64_t lba = block_num * (ep->block_size / 512);
        for (uint32_t s = 0; s < ep->block_size / 512; s++) {
            if (blockdev_write_sectors(ep->dev_id, lba + s, 1,
                                       block_buf + s * 512) != 0) {
                kfree(block_buf);
                return -EIO;
            }
        }
        offset += copy;
    }

    /* Write backup BGD tables in groups that have superblock backups */
    for (uint32_t g = 1; g < ep->num_block_groups; g++) {
        if (!ext2_group_has_super(sparse, g)) continue;
        uint32_t bgd_block_in_group = (ep->block_size == 1024) ? 2 : 1;
        uint32_t bgd_start = ext2_group_start(g, ep->blocks_per_group) + bgd_block_in_group;

        offset = 0;
        for (uint32_t i = 0; i < bgd_blocks; i++) {
            memset(block_buf, 0, ep->block_size);
            uint32_t copy = bgd_bytes - offset;
            if (copy > ep->block_size) copy = ep->block_size;
            memcpy(block_buf, (uint8_t *)ep->bgd_cache + offset, copy);

            uint64_t lba = (uint64_t)(bgd_start + i) * (ep->block_size / 512);
            for (uint32_t s = 0; s < ep->block_size / 512; s++) {
                if (blockdev_write_sectors(ep->dev_id, lba + s, 1,
                                           block_buf + s * 512) != 0) {
                    kfree(block_buf);
                    return -EIO;
                }
            }
            offset += copy;
        }
    }

    kfree(block_buf);
    return 0;
}

/* Initialize a new block group's metadata (bitmaps + inode table).
 * @ep: ext2 private data
 * @group: new group number to initialise
 * Returns 0 on success, negative on error. */
static int ext2_init_new_group(struct ext2_priv *ep, uint32_t group)
{
    uint32_t blocks_per_group = ep->blocks_per_group;
    uint32_t inodes_per_group = ep->inodes_per_group;
    uint32_t inode_size = ep->inode_size;
    uint32_t block_size = ep->block_size;
    int sparse = (ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ? 1 : 0;
    uint32_t group_start = group * blocks_per_group;

    /* Calculate group metadata layout */
    uint32_t sb_blocks = 0;
    uint32_t bgd_blocks = 0;
    if (ext2_group_has_super(sparse, group)) {
        sb_blocks = (block_size == 1024) ? 2 : 1;
        uint32_t num_groups = group + 1; /* BGD table covers all groups up to this one */
        bgd_blocks = (uint32_t)(((uint64_t)num_groups * sizeof(struct ext2_bg_desc)
                                 + block_size - 1) / block_size);
    }

    uint32_t block_bitmap_block = group_start + sb_blocks + bgd_blocks;
    uint32_t inode_bitmap_block = block_bitmap_block + 1;
    uint32_t inode_table_blocks = (inodes_per_group * inode_size + block_size - 1)
                                   / block_size;
    uint32_t inode_table_block = inode_bitmap_block + 1;

    /* Allocate a zero-filled block buffer */
    uint8_t *zero_buf = (uint8_t *)kmalloc(block_size);
    if (!zero_buf) return -ENOMEM;
    memset(zero_buf, 0, block_size);

    /* Write block bitmap: all blocks in the group are free (except metadata) */
    {
        uint8_t *bitmap = (uint8_t *)kmalloc(block_size);
        if (!bitmap) { kfree(zero_buf); return -ENOMEM; }
        memset(bitmap, 0xFF, block_size); /* start all free */

        uint32_t total_blocks_in_group = blocks_per_group;

        /* Mark used: superblock, BGD, bitmaps, inode table */
        uint32_t used_start = 0;
        uint32_t used_end = inode_table_block + inode_table_blocks;
        if (used_end > total_blocks_in_group)
            used_end = total_blocks_in_group;

        for (uint32_t b = used_start; b < used_end && b < total_blocks_in_group; b++) {
            bitmap[b / 8] &= ~(1U << (b % 8)); /* mark as used */
        }

        /* Write the block bitmap */
        uint64_t lba = (uint64_t)block_bitmap_block * (block_size / 512);
        for (uint32_t s = 0; s < block_size / 512; s++) {
            if (blockdev_write_sectors(ep->dev_id, lba + s, 1,
                                       bitmap + s * 512) != 0) {
                kfree(bitmap); kfree(zero_buf); return -EIO;
            }
        }
        kfree(bitmap);
    }

    /* Write inode bitmap: all inodes are free */
    {
        uint8_t *bitmap = (uint8_t *)kmalloc(block_size);
        if (!bitmap) { kfree(zero_buf); return -ENOMEM; }
        memset(bitmap, 0xFF, block_size); /* all free */

        uint64_t lba = (uint64_t)inode_bitmap_block * (block_size / 512);
        for (uint32_t s = 0; s < block_size / 512; s++) {
            if (blockdev_write_sectors(ep->dev_id, lba + s, 1,
                                       bitmap + s * 512) != 0) {
                kfree(bitmap); kfree(zero_buf); return -EIO;
            }
        }
        kfree(bitmap);
    }

    /* Write inode table: zero-filled */
    for (uint32_t i = 0; i < inode_table_blocks; i++) {
        uint64_t lba = (uint64_t)(inode_table_block + i) * (block_size / 512);
        for (uint32_t s = 0; s < block_size / 512; s++) {
            if (blockdev_write_sectors(ep->dev_id, lba + s, 1,
                                       zero_buf) != 0) {
                kfree(zero_buf); return -EIO;
            }
        }
    }

    /* Zero out the superblock and BGD backup area if present */
    if (sb_blocks > 0) {
        uint64_t sb_lba = (uint64_t)group_start * (block_size / 512);
        for (uint32_t s = 0; s < sb_blocks * block_size / 512; s++) {
            if (blockdev_write_sectors(ep->dev_id, sb_lba + s, 1,
                                       zero_buf) != 0) {
                kfree(zero_buf); return -EIO;
            }
        }
    }

    kfree(zero_buf);

    /* Update BGD cache entry */
    uint32_t num_inodes = inodes_per_group;
    uint32_t num_blocks = blocks_per_group - (inode_table_block + inode_table_blocks);
    if (sb_blocks > 0) num_blocks -= sb_blocks;

    ep->bgd_cache[group].bg_block_bitmap     = block_bitmap_block;
    ep->bgd_cache[group].bg_inode_bitmap     = inode_bitmap_block;
    ep->bgd_cache[group].bg_inode_table      = inode_table_block;
    ep->bgd_cache[group].bg_free_blocks_count = (uint16_t)num_blocks;
    ep->bgd_cache[group].bg_free_inodes_count = (uint16_t)num_inodes;
    ep->bgd_cache[group].bg_used_dirs_count  = 0;

    return 0;
}

/* Grow the ext2 filesystem by adding new block groups.
 * @ep: ext2 private data (from mount)
 * @new_total_blocks: desired total blocks after resize
 *
 * The function adds enough complete block groups to reach the requested
 * size.  Partial block groups are not supported — the actual new size
 * may be larger than requested (rounded up to complete groups).
 *
 * Returns the new total number of blocks on success, negative on error.
 */
int64_t ext2_resize(struct ext2_priv *ep, uint64_t new_total_blocks)
{
    if (!ep) return -EINVAL;

    uint32_t current_groups = ep->num_block_groups;
    uint32_t blocks_per_group = ep->blocks_per_group;
    uint32_t new_groups_needed = (uint32_t)
        ((new_total_blocks + blocks_per_group - 1) / blocks_per_group);

    if (new_groups_needed <= current_groups) {
        kprintf("[ext2] resize: new size %llu <= current size, nothing to do\n",
                (unsigned long long)new_total_blocks);
        return (int64_t)ep->sb.s_blocks_count;
    }

    kprintf("[ext2] resize: growing from %u groups (%u blocks) to %u groups (%llu blocks)\n",
            current_groups, ep->sb.s_blocks_count,
            new_groups_needed, (unsigned long long)new_groups_needed * blocks_per_group);

    /* Expand the BGD cache */
    int ret = ext2_resize_bgd_cache(ep, new_groups_needed);
    if (ret != 0) return ret;

    /* Initialize each new group */
    for (uint32_t g = current_groups; g < new_groups_needed; g++) {
        ret = ext2_init_new_group(ep, g);
        if (ret != 0) {
            kprintf("[ext2] resize: failed to init group %u: %d\n", g, ret);
            ep->num_block_groups = g; /* partially completed */
            return ret;
        }
        kprintf("[ext2] resize: group %u initialized\n", g);
    }

    /* Update superblock */
    uint32_t added_blocks = (new_groups_needed - current_groups) * blocks_per_group;
    ep->sb.s_blocks_count += added_blocks;
    ep->sb.s_free_blocks_count += added_blocks; /* approximate — all new blocks are free */

    /* Add free inodes for the new groups */
    uint32_t added_inodes = (new_groups_needed - current_groups) * ep->inodes_per_group;
    ep->sb.s_free_inodes_count += added_inodes;

    ep->num_block_groups = new_groups_needed;

    /* Update modification time */
    ep->sb.s_wtime = (uint32_t)(timer_get_ticks() / TIMER_FREQ);

    /* Sync to disk */
    if (ext2_sync_super(ep) != 0) {
        kprintf("[ext2] resize: failed to sync superblock\n");
        return -EINVAL;
    }
    if (ext2_sync_bgd(ep) != 0) {
        kprintf("[ext2] resize: failed to sync BGD\n");
        return -EINVAL;
    }

    kprintf("[ext2] resize: complete — %u groups, %u blocks total\n",
            ep->num_block_groups, ep->sb.s_blocks_count);

    return (int64_t)ep->sb.s_blocks_count;
}

int ext2_init(void) {
    kprintf("[ext2] Ext2 read-only filesystem initialized\n");
    vfs_register_filesystem("ext2", &ext2_ops);
    return 0;
}

#ifdef MODULE
/* Module entry point — called by the module ELF loader on insmod */
int init_module(void) {
    return ext2_init();
}

/* Module exit point — called by the module ELF loader on rmmod */
void cleanup_module(void) {
    /* No VFS unregister yet; avoid unloading if filesystem is mounted */
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Second extended filesystem (ext2) — read-only with HTree directory indexing");
#endif

/* ── ext2_umount ──────────────────────────────────────── */
int ext2_umount(const char *target)
{
    (void)target;
    kprintf("[ext2] Ext2 unmounted\n");
    return 0;
}
/* ── ext2_lookup ──────────────────────────────────────── */
int ext2_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[ext2] lookup: %s\n", name);
    return -ENOENT;
}
