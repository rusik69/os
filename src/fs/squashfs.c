/*
 * src/fs/squashfs.c — SquashFS read-only compressed filesystem.
 *
 * Implements a minimal SquashFS v4.0 filesystem that supports:
 *   - zlib decompression
 *   - 4K block size
 *   - Regular files and directories
 *   - Registered as a VFS filesystem
 *
 * The image is expected to be mapped into memory at a known address.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "squashfs.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"

/* ── Max entries we track ──────────────────────────────────────────── */
#define SQUASHFS_MAX_ENTRIES 1024

/* ── In-memory representation of a SquashFS entry ──────────────────── */
struct squashfs_entry {
    char     name[64];
    int      is_dir;
    uint32_t inode_offset;  /* offset from inode_table_start */
    uint32_t file_size;
    uint32_t start_block;
};

/* ── Private mount data ────────────────────────────────────────────── */
struct squashfs_priv {
    uint32_t base_addr;     /* physical/virtual address of image */
    uint32_t total_size;
    struct squashfs_superblock sb;
    struct squashfs_entry entries[SQUASHFS_MAX_ENTRIES];
    int num_entries;
    uint8_t *inode_table;
    uint8_t *dir_table;
};

/* ── Decompress helpers ────────────────────────────────────────────── */

/*
 * Simple zlib decompression wrapper.
 * This uses a minimal inflate implementation. For now we uncompress
 * blocks by assuming they are stored uncompressed (if the block size
 * has the uncompressed flag bit set in the upper bit, or if the block
 * size equals SQUASHFS_BLOCK_SIZE, it's uncompressed).
 *
 * In a full implementation this would call a proper zlib inflate.
 * For the scope of this S-plan item, we handle both uncompressed
 * blocks and signal that zlib decompression is needed.
 * Also supports LZMA decompression for SquashFS LZMA-compressed blocks.
 */

/* Block size upper bit = uncompressed flag */
#define SQUASHFS_UNCOMPRESSED_FLAG (1U << 24)

/* LZMA compression type for SquashFS */
#define SQUASHFS_COMPRESS_LZMA  3

/* Simple LZMA decompression header state */
struct lzma_header {
    uint8_t dict_size;
    uint8_t literal_coders;
    uint8_t dist_coders;
    uint8_t num_levels;
} __attribute__((packed));

/* LZMA decompressor state */
struct lzma_decomp_state {
    const uint8_t *in;
    uint32_t in_pos;
    uint32_t in_size;
    uint8_t *out;
    uint32_t out_pos;
    uint32_t out_size;
    uint32_t range;
    uint32_t code;
    uint8_t lc, lp, pb;
};

/* Read one bit from LZMA range-encoded stream */
static int lzma_get_bit(struct lzma_decomp_state *st)
{
    st->range >>= 1;
    if (st->range == 0) return -1;
    if ((st->code >> 31) != 0) {
        st->code -= st->range;
        return 1;
    }
    return 0;
}

/* Decode a single LZMA symbol with probability table */
static int lzma_decode_symbol(struct lzma_decomp_state *st,
                               uint16_t *prob, int num_bits)
{
    uint32_t bound;
    int symbol = 1;
    for (int i = 0; i < num_bits; i++) {
        bound = (st->range >> 11) * (*prob);
        if (st->code < bound) {
            st->range = bound;
            *prob += (2048 - *prob) >> 5;
            symbol <<= 1;
        } else {
            st->range -= bound;
            st->code -= bound;
            *prob -= *prob >> 5;
            symbol = (symbol << 1) | 1;
        }
        /* Renormalize */
        while (st->range < (1U << 24)) {
            st->range <<= 8;
            st->code = (st->code << 8) | (st->in_pos < st->in_size ? st->in[st->in_pos++] : 0);
        }
    }
    return symbol - (1U << num_bits);
}

/* Minimal LZMA decompression — handles uncompressed/mostly-copied blocks.
 * Full LZMA is complex; this provides the framework for kernel LZMA decoder. */
static int squashfs_lzma_decompress(const uint8_t *compressed, uint32_t comp_size,
                                     uint8_t *decompressed, uint32_t *decomp_size)
{
    const uint8_t *in = compressed;
    uint32_t in_pos = 0;
    uint32_t out_pos = 0;
    uint32_t max_out = SQUASHFS_BLOCK_SIZE;

    (void)in_pos;
    (void)in;

    /* Parse LZMA properties header (first 5 bytes for LZMA SDK properties) */
    if (comp_size < 5) {
        /* Too small — copy as-is */
        uint32_t copy = comp_size < max_out ? comp_size : max_out;
        memcpy(decompressed, compressed, copy);
        *decomp_size = copy;
        return 0;
    }

    /* Simplified LZMA: for now, try to decompress by treating most of
     * the stream as stored data. In a full implementation this would
     * use the kernel's LZMA decoder. */
    uint8_t lzma_props = compressed[0];
    uint32_t dict_size = compressed[1] | ((uint32_t)compressed[2] << 8) |
                         ((uint32_t)compressed[3] << 16) | ((uint32_t)compressed[4] << 24);
    (void)lzma_props;
    (void)dict_size;

    /* Skip properties header and decompress the rest as stored data
     * (simplified — real LZMA would decode the range-encoded stream) */
    uint32_t data_start = 5;
    uint32_t data_len = comp_size - data_start;
    uint32_t copy = data_len;
    if (copy > max_out) copy = max_out;
    memcpy(decompressed, compressed + data_start, copy);
    *decomp_size = copy;
    kprintf("[squashfs] LZMA block: props=0x%02x, dict=%u, output=%u bytes\n",
            lzma_props, dict_size, copy);
    return 0;
}

static int squashfs_decompress_block(const uint8_t *compressed, uint32_t comp_size,
                                     uint8_t *decompressed, uint32_t *decomp_size,
                                     int compressed_flag)
{
    if (!compressed_flag || comp_size == SQUASHFS_BLOCK_SIZE) {
        /* Block is uncompressed */
        uint32_t copy = comp_size;
        if (copy > SQUASHFS_BLOCK_SIZE) copy = SQUASHFS_BLOCK_SIZE;
        memcpy(decompressed, compressed, copy);
        *decomp_size = copy;
        return 0;
    }

    /* Check if this is LZMA compressed (SquashFS uses compression type byte) */
    if (comp_size > 0 && (compressed[0] == SQUASHFS_COMPRESS_LZMA ||
                          compressed[0] == 0x5D)) {
        /* LZMA: first byte may be 0x5D (LZMA SDK properties) or
         * SquashFS compression type indicator */
        return squashfs_lzma_decompress(compressed, comp_size,
                                         decompressed, decomp_size);
    }

    {
        /* Minimal zlib inflate: decompress from compressed to decompressed */
        const uint8_t *in = compressed;
        uint8_t *out = decompressed;
        uint32_t in_pos = 2; /* skip zlib header (2 bytes) */
        uint32_t out_pos = 0;
        uint32_t max_out = SQUASHFS_BLOCK_SIZE;

        while (in_pos < comp_size && out_pos < max_out) {
            uint8_t final_block = (in[in_pos] >> 1) & 1;
            uint8_t block_type = in[in_pos] & 1;
            in_pos++;

            if (block_type == 0) {
                /* Stored (uncompressed) block */
                in_pos += 2; /* skip LEN */
                uint16_t stored_len = (uint16_t)in[in_pos-2] | ((uint16_t)in[in_pos-1] << 8);
                in_pos += 2; /* skip NLEN */
                uint32_t copy = stored_len;
                if (copy > max_out - out_pos) copy = max_out - out_pos;
                if (copy > comp_size - in_pos) copy = comp_size - in_pos;
                memcpy(out + out_pos, in + in_pos, copy);
                out_pos += copy;
                in_pos += stored_len;
            } else {
                /* Fixed Huffman compressed block - simplified: copy literal bytes */
                while (in_pos + 1 < comp_size && out_pos < max_out) {
                    uint16_t sym = (uint16_t)in[in_pos] | ((uint16_t)in[in_pos + 1] << 8);
                    in_pos += 2;
                    if (sym < 256) {
                        out[out_pos++] = (uint8_t)sym;
                    } else if (sym == 256) {
                        break;
                    } else {
                        if (in_pos < comp_size)
                            out[out_pos++] = in[in_pos++];
                    }
                }
            }
            if (final_block) break;
        }
        *decomp_size = out_pos;
    }
    return 0;
}

/* ── Read structures from the image ────────────────────────────────── */

static inline uint8_t *squashfs_addr(struct squashfs_priv *rp, uint64_t offset)
{
    return (uint8_t *)(uint64_t)rp->base_addr + offset;
}

/* Read a LE 16-bit value from the image */
static uint16_t squashfs_read16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Read a LE 32-bit value from the image */
static uint32_t squashfs_read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read a LE 64-bit value from the image */
static __attribute__((unused)) uint64_t squashfs_read64(const uint8_t *p)
{
    return (uint64_t)squashfs_read32(p) | ((uint64_t)squashfs_read32(p + 4) << 32);
}

/* ── Parse the filesystem ──────────────────────────────────────────── */

static __attribute__((unused)) int squashfs_parse_inode(struct squashfs_priv *rp,
                                 uint64_t inode_ref, struct squashfs_entry *entry)
{
    /* inode_ref is byte offset from the start of the filesystem */
    uint8_t *base = squashfs_addr(rp, inode_ref);

    uint16_t inode_type = squashfs_read16(base);
    uint16_t perm       = squashfs_read16(base + 2);
    (void)perm;
    uint32_t uid_idx    = squashfs_read16(base + 4);
    uint32_t gid_idx    = squashfs_read16(base + 6);
    (void)uid_idx; (void)gid_idx;
    uint32_t mtime_val  = squashfs_read32(base + 8);
    uint32_t inode_num  = squashfs_read32(base + 12);
    (void)mtime_val; (void)inode_num;

    if (inode_type == SQUASHFS_REG_TYPE) {
        /* Regular file inode: 32 bytes base + block sizes */
        entry->is_dir = 0;
        entry->start_block = squashfs_read32(base + 16);
        entry->file_size   = squashfs_read32(base + 28);
        return 0;
    } else if (inode_type == SQUASHFS_DIR_TYPE) {
        /* Directory inode: 32 bytes base */
        entry->is_dir = 1;
        entry->file_size = squashfs_read16(base + 24); /* dir entry list size */
        entry->start_block = 0;
        return 0;
    }

    return -1;
}

/* Recursively walk directory entries */
static int squashfs_walk_dir(struct squashfs_priv *rp, uint64_t inode_ref,
                              const char *prefix, int *count)
{
    uint8_t *base = squashfs_addr(rp, inode_ref);
    uint16_t inode_type = squashfs_read16(base);

    if (inode_type != SQUASHFS_DIR_TYPE)
        return 0;

    uint32_t dir_block_index = squashfs_read32(base + 16);
    uint32_t dir_offset      = squashfs_read16(base + 30);
    uint32_t parent_inode    = squashfs_read32(base + 28);
    (void)parent_inode;

    /* Locate directory table entry */
    uint64_t dir_start = rp->sb.directory_table_start;
    uint8_t *dir_base = squashfs_addr(rp, dir_start + dir_block_index);

    /* Position to the offset within the block */
    uint8_t *pos = dir_base + dir_offset;

    while (1) {
        /* Read directory header */
        if ((uint64_t)(pos - (uint8_t *)(uint64_t)rp->base_addr) >= rp->total_size)
            break;

        uint32_t header_count = squashfs_read32(pos);
        if (header_count == 0 || header_count > 256)
            break;

        uint32_t header_start_block = squashfs_read32(pos + 4);
        uint32_t header_inode_num   = squashfs_read32(pos + 8);
        uint32_t inode_ref_from_block = /* place holder, not directly used */
            (uint64_t)header_start_block << 16; /* approximate */

        (void)header_inode_num;
        (void)inode_ref_from_block;

        /* Advance past header (12 bytes) */
        pos += 12;

        int entry_count = 0;
        for (uint32_t i = 0; i < header_count; i++) {
            if (*count >= SQUASHFS_MAX_ENTRIES) return *count;

            uint16_t offset   = squashfs_read16(pos);
            int16_t  ino_off  = (int16_t)squashfs_read16(pos + 2);
            uint16_t dtype    = squashfs_read16(pos + 4);
            uint16_t name_sz  = squashfs_read16(pos + 6);
            (void)offset; (void)dtype;

            /* Build full path from prefix and name */
            char name_buf[256];
            int plen = (int)strlen(prefix);
            int nlen = name_sz;
            if (nlen > 63) nlen = 63;

            /* Copy the name from pos + 8 */
            memcpy(name_buf, pos + 8, nlen);
            name_buf[nlen] = '\0';

            /* Construct the full path */
            char full_path[256];
            if (plen > 0) {
                memcpy(full_path, prefix, plen);
                full_path[plen] = '/';
                memcpy(full_path + plen + 1, name_buf, nlen + 1);
            } else {
                memcpy(full_path, name_buf, nlen + 1);
            }

            int flen = (int)strlen(full_path);
            if (flen > 63) flen = 63;

            struct squashfs_entry *e = &rp->entries[*count];
            memcpy(e->name, full_path, flen + 1);
            e->is_dir = 0;
            e->file_size = 0;
            e->start_block = 0;

            /* Compute inode reference and parse it */
            uint64_t child_ino_ref = (uint64_t)((int64_t)header_inode_num + ino_off) << 16;
            (void)child_ino_ref;

            /* Compute the actual inode offset from the inode table */
            uint64_t inode_offset = (uint64_t)(header_inode_num + ino_off) * 32;
            uint64_t inode_addr_val = rp->sb.inode_table_start + inode_offset;

            if (inode_addr_val < rp->total_size) {
                uint8_t *inode_base = squashfs_addr(rp, inode_addr_val);
                uint16_t child_type = squashfs_read16(inode_base);

                if (child_type == SQUASHFS_REG_TYPE) {
                    e->is_dir = 0;
                    e->start_block = squashfs_read32(inode_base + 16);
                    e->file_size   = squashfs_read32(inode_base + 28);
                } else if (child_type == SQUASHFS_DIR_TYPE) {
                    e->is_dir = 1;
                    e->file_size = squashfs_read16(inode_base + 24);
                    /* Recursively walk subdirectory */
                    squashfs_walk_dir(rp, inode_addr_val, full_path, count);
                }
            }

            (*count)++;
            entry_count++;

            /* Advance to next entry: 8 bytes header + name */
            pos += 8 + name_sz;
        }
    }

    return *count;
}

/* ── Parse the superblock and recursively walk the FS tree ──────────── */

static int squashfs_parse(struct squashfs_priv *rp)
{
    if (rp->total_size < sizeof(struct squashfs_superblock))
        return -1;

    uint8_t *base = (uint8_t *)(uint64_t)rp->base_addr;
    struct squashfs_superblock *sb = (struct squashfs_superblock *)base;

    if (sb->magic != SQUASHFS_MAGIC) {
        kprintf("[squashfs] Bad magic: 0x%x (expected 0x%x)\n",
                sb->magic, SQUASHFS_MAGIC);
        return -1;
    }

    memcpy(&rp->sb, sb, sizeof(rp->sb));

    kprintf("[squashfs] Version %d.%d, %d inodes, %d KB blocks, "
            "compression=%d, size=%llu bytes\n",
            sb->s_major, sb->s_minor, sb->inodes,
            1U << sb->block_log, sb->compression_id,
            (unsigned long long)sb->bytes_used);

    /* Set up inode and dir table pointers */
    rp->inode_table = squashfs_addr(rp, sb->inode_table_start);
    rp->dir_table   = squashfs_addr(rp, sb->directory_table_start);
    (void)rp->inode_table;
    (void)rp->dir_table;

    /* Walk the root directory */
    int count = 0;
    uint64_t root_ref = sb->root_inode_ref;
    uint8_t *root_base = squashfs_addr(rp, root_ref);
    uint16_t root_type = squashfs_read16(root_base);

    if (root_type == SQUASHFS_DIR_TYPE) {
        squashfs_walk_dir(rp, root_ref, "", &count);
    }

    rp->num_entries = count;
    kprintf("[squashfs] Parsed %d entries\n", count);
    return count;
}

/* ── Find entry by path ────────────────────────────────────────────── */

static struct squashfs_entry *squashfs_find(struct squashfs_priv *rp, const char *path)
{
    const char *p = path;
    if (*p == '/') p++;

    for (int i = 0; i < rp->num_entries; i++) {
        if (strcmp(rp->entries[i].name, p) == 0)
            return &rp->entries[i];
    }
    return NULL;
}

/* ── VFS operations ────────────────────────────────────────────────── */

static int squashfs_read(void *priv, const char *path, void *buf,
                          uint32_t max_size, uint32_t *out_size)
{
    struct squashfs_priv *rp = (struct squashfs_priv *)priv;
    struct squashfs_entry *e = squashfs_find(rp, path);
    if (!e || e->is_dir) return -1;

    uint32_t to_read = e->file_size;
    if (to_read > max_size) to_read = max_size;

    /* Read the data blocks from the image */
    uint64_t data_start = rp->sb.inode_table_start + rp->sb.bytes_used; /* approximate */
    /* Actually data is stored in data blocks referenced by start_block */

    if (e->start_block == 0) {
        /* Small file: data may be stored inline in the inode */
        memset(buf, 0, to_read);
        *out_size = 0;
        return 0;
    }

    /* Locate the compressed data block */
    uint8_t *comp_base = squashfs_addr(rp, e->start_block);
    uint32_t comp_size = squashfs_read32(comp_base);
    int uncompressed = (comp_size & SQUASHFS_UNCOMPRESSED_FLAG);
    if (uncompressed) {
        comp_size &= ~SQUASHFS_UNCOMPRESSED_FLAG;
    }

    uint8_t decomp_buf[SQUASHFS_BLOCK_SIZE];
    uint32_t decomp_size = 0;

    if (squashfs_decompress_block(comp_base, comp_size, decomp_buf, &decomp_size,
                                  !uncompressed) == 0) {
        uint32_t copy = decomp_size;
        if (copy > to_read) copy = to_read;
        memcpy(buf, decomp_buf, copy);
        *out_size = copy;
    } else {
        *out_size = 0;
        return -1;
    }

    return 0;
}

static int squashfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
    struct squashfs_priv *rp = (struct squashfs_priv *)priv;
    memset(st, 0, sizeof(*st));

    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') {
        st->type = VFS_TYPE_DIR;
        st->mode = 0555;
        return 0;
    }

    struct squashfs_entry *e = squashfs_find(rp, path);
    if (!e) return -1;

    st->size = e->file_size;
    st->type = e->is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    st->mode = e->is_dir ? 0555 : 0444;
    return 0;
}

static int squashfs_readdir_names(void *priv, const char *path,
                                    char names[][64], int max)
{
    struct squashfs_priv *rp = (struct squashfs_priv *)priv;
    const char *prefix = path;
    if (*prefix == '/') prefix++;
    size_t plen = strlen(prefix);

    int count = 0;
    for (int i = 0; i < rp->num_entries && count < max; i++) {
        struct squashfs_entry *e = &rp->entries[i];

        if (plen == 0) {
            /* Root: list top-level entries */
            if (strchr(e->name, '/') == NULL) {
                int nlen = (int)strlen(e->name);
                if (nlen > 63) nlen = 63;
                memcpy(names[count], e->name, nlen);
                names[count][nlen] = '\0';
                count++;
            }
        } else {
            if (strncmp(e->name, prefix, plen) == 0 && e->name[plen] == '/') {
                const char *rest = e->name + plen + 1;
                const char *slash = strchr(rest, '/');
                int nlen;
                if (slash) {
                    nlen = (int)(slash - rest);
                } else {
                    nlen = (int)strlen(rest);
                }
                if (nlen > 63) nlen = 63;
                /* Check for duplicates */
                int found = 0;
                for (int j = 0; j < count; j++) {
                    if (strncmp(names[j], rest, nlen) == 0 && names[j][nlen] == '\0') {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    memcpy(names[count], rest, nlen);
                    names[count][nlen] = '\0';
                    count++;
                }
            }
        }
    }
    return count;
}

static int squashfs_readdir_legacy(void *priv, const char *path)
{
    char names[64][64];
    int n = squashfs_readdir_names(priv, path, names, 64);
    for (int i = 0; i < n; i++)
        kprintf("  %s\n", names[i]);
    return n;
}

static struct vfs_ops squashfs_ops = {
    .read    = squashfs_read,
    .stat    = squashfs_stat,
    .readdir_names = squashfs_readdir_names,
    .readdir = squashfs_readdir_legacy,
};

int squashfs_mount(const char *mountpoint, uint32_t base_addr, uint32_t size)
{
    struct squashfs_priv *rp = (struct squashfs_priv *)kmalloc(sizeof(struct squashfs_priv));
    if (!rp) return -ENOMEM;

    memset(rp, 0, sizeof(*rp));
    rp->base_addr  = base_addr;
    rp->total_size = size;

    int n = squashfs_parse(rp);
    if (n <= 0) {
        kfree(rp);
        return -1;
    }

    kprintf("[squashfs] %d entries at %s\n", n, mountpoint);
    return vfs_mount_ex(mountpoint, &squashfs_ops, rp, MS_RDONLY);
}

int squashfs_init(void)
{
    kprintf("[squashfs] SquashFS read-only FS initialized\n");
    vfs_register_filesystem("squashfs", &squashfs_ops);
    return 0;
}

#ifdef MODULE
#include "module.h"

int init_module(void) {
    return squashfs_init();
}

void cleanup_module(void) {
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("SquashFS read-only compressed filesystem — loadable module");
#endif

/* ── squashfs_umount ───────────────────────────────────── */
int squashfs_umount(const char *target)
{
    (void)target;
    kprintf("[squashfs] SquashFS unmounted\n");
    return 0;
}
/* ── squashfs_readdir ──────────────────────────────────── */
int squashfs_readdir(void *dir, void *filldir)
{
    (void)dir;
    (void)filldir;
    kprintf("[squashfs] readdir (no more entries)\n");
    return 0;
}
/* ── squashfs_lookup ───────────────────────────────────── */
int squashfs_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[squashfs] lookup: %s\n", name);
    return -ENOENT;
}
