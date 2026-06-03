/*
 * src/fs/ext2.c — Ext2 read-only filesystem with HTree directory indexing.
 *
 * Implements a read-only ext2 filesystem on top of the VFS layer.
 * Supports block groups, inodes, directory traversal (linear and
 * HTree/indexed), and reading regular files via direct/indirect blocks.
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
};

/* Read one block from the block device */
static int ext2_read_block(struct ext2_priv *ep, uint32_t block_num, uint8_t *buf) {
    uint64_t lba = (uint64_t)block_num * (ep->block_size / 512);
    uint32_t sectors = ep->block_size / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(ep->dev_id, lba + i, 1, buf + i * 512) != 0)
            return -1;
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
            return -1;
    }
    memcpy(&ep->sb, buf, sizeof(ep->sb));
    return 0;
}

/* Read inode */
static int ext2_read_inode(struct ext2_priv *ep, uint32_t ino, struct ext2_inode *inode) {
    if (ino == 0) return -1;
    uint32_t group = (ino - 1) / ep->inodes_per_group;
    uint32_t index = (ino - 1) % ep->inodes_per_group;

    /* Read block group descriptor */
    uint32_t bgd_block = (ep->block_size == 1024) ? 2 : 1;
    uint8_t bgd_buf[32];
    if (ext2_read_block(ep, bgd_block, bgd_buf) < 0) return -1;

    struct ext2_bg_desc *bgd = (struct ext2_bg_desc *)(bgd_buf + 0);
    /* For multiple block groups, we'd need to read the correct one */
    (void)group;

    uint32_t inode_table_block = bgd->bg_inode_table;
    uint32_t byte_offset = index * ep->inode_size;

    uint32_t tbl_block = inode_table_block + byte_offset / ep->block_size;
    uint32_t tbl_off   = byte_offset % ep->block_size;

    uint8_t block_buf[4096];
    if (ep->block_size > 4096) return -1;
    if (ext2_read_block(ep, tbl_block, block_buf) < 0) return -1;

    memcpy(inode, block_buf + tbl_off, sizeof(struct ext2_inode));
    return 0;
}

/* Read block from inode (handles indirect blocks) */
static int ext2_read_inode_block(struct ext2_priv *ep, struct ext2_inode *inode,
                                  uint32_t iblock, uint8_t *buf) {
    if (iblock < 12) {
        /* Direct block */
        if (inode->i_block[iblock] == 0) return -1;
        return ext2_read_block(ep, inode->i_block[iblock], buf);
    }

    /* Singly indirect */
    uint32_t entries_per_block = ep->block_size / 4;
    uint32_t sind = iblock - 12;

    if (sind < entries_per_block) {
        if (inode->i_block[12] == 0) return -1;
        uint8_t indir[4096];
        if (ext2_read_block(ep, inode->i_block[12], indir) < 0) return -1;
        uint32_t *ptrs = (uint32_t *)indir;
        if (ptrs[sind] == 0) return -1;
        return ext2_read_block(ep, ptrs[sind], buf);
    }

    return -1; /* doubly/triply indirect not needed for basic support */
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
        return -1;

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
            return -1;
        pos += *de_rec;
    }

    /* Skip '..' entry */
    {
        uint32_t *de_inode  = (uint32_t *)(block_buf + pos);
        uint16_t *de_rec    = (uint16_t *)(block_buf + pos + 4);
        if (*de_inode == 0 || *de_rec == 0)
            return -1;
        pos += *de_rec;
    }

    /* At pos, we should have: reserved(4) + hash_version(1) + info_length(1)
     * + indirect_levels(1) + unused_flags(1) = 8 bytes.  Then
     * limit(2) + count(2) + block(4) = 8 bytes.
     * So entries start at pos + 16. */

    if (pos + 16 > ep->block_size)
        return -1;

    /* Read the info fields */
    uint8_t info_bytes[8];
    memcpy(info_bytes, block_buf + pos, 8);
    uint8_t hash_version    = info_bytes[4];   /* offset 4 within the 8-byte info */
    uint8_t info_length     = info_bytes[5];
    uint8_t indirect_levels = info_bytes[6];
    (void)hash_version;

    if (info_length < 8)
        return -1;

    /* Read limit, count, block from the root node header */
    uint16_t root_limit = *(uint16_t *)(block_buf + pos + 8);
    uint16_t root_count = *(uint16_t *)(block_buf + pos + 10);
    uint32_t root_block = *(uint32_t *)(block_buf + pos + 12);
    (void)root_limit;

    if (root_count == 0)
        return -1;

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
            if (!ibuf) return -1;
            if (ext2_read_block(ep, current_block, ibuf) < 0) {
                kfree(ibuf);
                return -1;
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
            return -1;
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

    return -1;
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
        return -1;

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

    while (offset < dir_inode->i_size) {
        uint8_t block_buf[4096];
        if (ext2_read_inode_block(ep, dir_inode, iblock, block_buf) < 0)
            break;

        uint32_t pos = 0;
        while (pos < ep->block_size && offset + pos < dir_inode->i_size) {
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

    return -1;
}

/* Read directory entries from inode (linear, returns max entries). */
static int ext2_read_dir(struct ext2_priv *ep, struct ext2_inode *inode,
                          char names[][64], int max) {
    uint32_t iblock = 0;
    uint32_t offset = 0;
    int count = 0;

    while (offset < inode->i_size && count < max) {
        uint8_t block_buf[4096];
        if (ext2_read_inode_block(ep, inode, iblock, block_buf) < 0)
            break;

        uint32_t pos = 0;
        while (pos < ep->block_size && offset + pos < inode->i_size && count < max) {
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
        if (ext2_read_inode(ep, *ino, &dir_inode) < 0) return -1;

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
            return -1;

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
    if (ext2_path_to_ino(ep, path, &ino) < 0) return -1;

    struct ext2_inode inode;
    if (ext2_read_inode(ep, ino, &inode) < 0) return -1;

    uint32_t to_read = inode.i_size;
    if (to_read > max_size) to_read = max_size;

    uint32_t iblock = 0;
    uint32_t done = 0;
    while (done < to_read) {
        uint8_t block_buf[4096];
        if (ext2_read_inode_block(ep, &inode, iblock, block_buf) < 0)
            break;

        uint32_t chunk = to_read - done;
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
    if (ext2_path_to_ino(ep, path, &ino) < 0) return -1;

    struct ext2_inode inode;
    if (ext2_read_inode(ep, ino, &inode) < 0) return -1;

    memset(st, 0, sizeof(*st));
    st->size = inode.i_size;
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
    if (ext2_path_to_ino(ep, path, &ino) < 0) return -1;

    struct ext2_inode inode;
    if (ext2_read_inode(ep, ino, &inode) < 0) return -1;

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
    if (!ep) return -1;

    memset(ep, 0, sizeof(*ep));
    ep->dev_id = dev_id;

    if (ext2_load_super(ep) < 0) {
        kfree(ep);
        return -1;
    }

    if (ep->sb.s_magic != EXT2_SUPER_MAGIC) {
        kprintf("[ext2] Bad superblock magic: 0x%x\n", ep->sb.s_magic);
        kfree(ep);
        return -1;
    }

    ep->block_size = 1024 << ep->sb.s_log_block_size;
    if (ep->block_size > 4096) {
        kprintf("[ext2] Block size %u too large\n", ep->block_size);
        kfree(ep);
        return -1;
    }

    ep->blocks_per_group = ep->sb.s_blocks_per_group;
    ep->inodes_per_group = ep->sb.s_inodes_per_group;
    ep->inode_size = 128; /* standard */

    uint32_t total_groups = (ep->sb.s_blocks_count + ep->blocks_per_group - 1) / ep->blocks_per_group;
    ep->num_block_groups = total_groups;

    kprintf("[ext2] Mounted: %u blocks, %u inodes, %u B/block, %u groups\n",
            ep->sb.s_blocks_count, ep->sb.s_inodes_count,
            ep->block_size, total_groups);

    return vfs_mount_ex(mountpoint, &ext2_ops, ep, MS_RDONLY);
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
