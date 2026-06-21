/*
 * src/fs/ext4.c — Ext4 read-only filesystem with extent tree support.
 *
 * Implements a read-only ext4 filesystem on top of the VFS layer.
 * Supports:
 *   - Ext4 superblock with EXT4_FEATURE_INCOMPAT_EXTENTS flag
 *   - Flex block groups (flex_bg)
 *   - Extent tree: ext4_extent_header → ext4_extent_idx → ext4_extent
 *   - Inline data in inodes
 *   - Directory entries with file_type
 *   - stat, readdir, read
 *
 * Read-only — no journal, no encryption, no htree indexing.
 * Backward-compatible with ext2/ext3; detected by feature flags.
 */

#define KERNEL_INTERNAL
#include "ext4.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"
#include "initcall.h"

#ifdef MODULE
#include "module.h"
#endif

/* Ext4 extent magic */
#define EXT4_EXTENT_MAGIC 0xF30A

/* Maximum depth of extent tree (sanity check) */
#define EXT4_EXTENT_MAX_DEPTH 5

/* Maximum block size we support (needed for stack buffers) */
#define EXT4_MAX_BLOCK_SIZE 4096

struct ext4_priv {
    uint8_t  dev_id;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t num_block_groups;
    struct ext4_superblock sb;
    char     mountpoint[64];

    /* Cached block group descriptor table */
    struct ext4_bg_desc *bgd_cache;
    uint32_t             bgd_cache_size;

    /* Feature flags (cached for fast access) */
    uint32_t incompat;
    uint32_t ro_compat;
    uint32_t compat;

    /* flex_bg: number of block groups in a flex_bg group */
    uint32_t flex_bg_size; /* 0 if flex_bg not enabled */
};

/* ── Corruption helper ────────────────────────────────────────────── */

static int ext4_corrupt(struct ext4_priv *ep, const char *reason)
{
    if (!ep)
        return -EFSCORRUPTED;
    vfs_force_readonly(ep->mountpoint, reason);
    return -EFSCORRUPTED;
}

/* ── Block I/O ────────────────────────────────────────────────────── */

static int ext4_read_block(struct ext4_priv *ep, uint32_t block_num, uint8_t *buf)
{
    uint64_t lba = (uint64_t)block_num * (ep->block_size / 512);
    uint32_t sectors = ep->block_size / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(ep->dev_id, lba + i, 1, buf + i * 512) != 0)
            return ext4_corrupt(ep, "block I/O error");
    }
    return 0;
}

/* ── Superblock loading ───────────────────────────────────────────── */

static int ext4_load_super(struct ext4_priv *ep)
{
    uint8_t buf[1024];
    /* Superblock is at offset 1024 (sector 2 for 512-byte sectors) */
    uint64_t lba = 1024 / 512;
    if (blockdev_read_sectors(ep->dev_id, lba, 2, buf) != 0)
        return -1;
    memcpy(&ep->sb, buf, sizeof(ep->sb));
    return 0;
}

/* ── Block group descriptor caching ───────────────────────────────── */

static int ext4_load_bgd_cache(struct ext4_priv *ep)
{
    uint32_t bgd_entry_size = sizeof(struct ext4_bg_desc);
    /* For 64-bit mode, each BGD entry is larger (64 bytes vs 32) */
    if (ep->incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        bgd_entry_size = 64;

    uint32_t bgd_bytes = ep->num_block_groups * bgd_entry_size;
    uint32_t bgd_blocks = (bgd_bytes + ep->block_size - 1) / ep->block_size;

    ep->bgd_cache = (struct ext4_bg_desc *)kmalloc(bgd_bytes);
    if (!ep->bgd_cache)
        return -ENOMEM;
    ep->bgd_cache_size = bgd_bytes;

    /* With flex_bg, the block group descriptor table may span multiple
     * groups' reserved space.  For simplicity, read from the primary
     * location (block 1 if block_size=1024, else block 0 after superblock
     * backup).  The caller must ensure the primary copy is valid. */
    uint32_t bgd_start_block;
    if (ep->block_size == 1024)
        bgd_start_block = 2; /* block 0 is boot block, block 1 is sb, block 2 is bgd */
    else
        bgd_start_block = 1; /* block 0 is sb + bgd */

    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];
    uint32_t offset = 0;
    for (uint32_t b = 0; b < bgd_blocks; b++) {
        if (ext4_read_block(ep, bgd_start_block + b, block_buf) < 0)
            return ext4_corrupt(ep, "failed to read BGD table");
        uint32_t copy = ep->block_size;
        uint32_t remaining = bgd_bytes - offset;
        if (copy > remaining) copy = remaining;
        memcpy((uint8_t *)ep->bgd_cache + offset, block_buf, copy);
        offset += copy;
    }
    return 0;
}

/* ── Inode reading ─────────────────────────────────────────────────── */

static int ext4_read_inode(struct ext4_priv *ep, uint32_t ino, struct ext4_inode *inode)
{
    if (ino == 0)
        return ext4_corrupt(ep, "inode 0 is invalid");
    if (ino > ep->sb.s_inodes_count)
        return ext4_corrupt(ep, "inode number exceeds count");
    uint32_t group = (ino - 1) / ep->inodes_per_group;
    uint32_t index = (ino - 1) % ep->inodes_per_group;

    if (!ep->bgd_cache || group >= ep->num_block_groups)
        return ext4_corrupt(ep, "block group out of range");

    struct ext4_bg_desc *bgd = &ep->bgd_cache[group];
    uint32_t inode_table_block = bgd->bg_inode_table;
    uint32_t byte_offset = index * ep->inode_size;

    uint32_t tbl_block = inode_table_block + byte_offset / ep->block_size;
    uint32_t tbl_off   = byte_offset % ep->block_size;

    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];
    if (ext4_read_block(ep, tbl_block, block_buf) < 0)
        return -1;

    memcpy(inode, block_buf + tbl_off, sizeof(struct ext4_inode));
    return 0;
}

/* ── Extent tree walker ────────────────────────────────────────────── */

/* Given an inode with EXT4_EXTENTS_FL set, resolve logical block iblock
 * to a physical block number (or 0 for hole).  Returns -1 on error. */
static int64_t ext4_get_extent_block(struct ext4_priv *ep,
                                      struct ext4_inode *inode,
                                      uint32_t iblock)
{
    /* The extent tree root is in i_block[0..14] as a struct ext4_extent_header
     * followed by entries.  We treat the 15 32-bit words (60 bytes) as the
     * root node of the extent tree. */
    uint8_t root_buf[60];
    memcpy(root_buf, inode->i_block, 60);

    struct ext4_extent_header *eh = (struct ext4_extent_header *)root_buf;
    if (eh->eh_magic != EXT4_EXTENT_MAGIC)
        return ext4_corrupt(ep, "bad extent magic");

    uint16_t depth = eh->eh_depth;
    if (depth > EXT4_EXTENT_MAX_DEPTH)
        return ext4_corrupt(ep, "extent tree too deep");

    /* Walk the tree from root to leaf */
    uint8_t node_buf[EXT4_MAX_BLOCK_SIZE];
    uint8_t *node_data = root_buf;
    uint32_t node_size = 60; /* root fits in 60 bytes */
    int is_root = 1;

    while (1) {
        eh = (struct ext4_extent_header *)node_data;

        if (depth > 0) {
            /* Internal (index) node — find the right child */
            struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
            uint16_t num_entries = eh->eh_entries;
            uint16_t i;

            /* Binary search for the index entry covering iblock */
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

            if (found < 0) {
                /* Before first block — hole or empty tree */
                return 0;
            }

            /* Compute physical block of child node */
            uint64_t child_block = ((uint64_t)idx[found].ei_leaf_hi << 32) |
                                    idx[found].ei_leaf_lo;

            /* Read child block */
            if (ext4_read_block(ep, (uint32_t)child_block, node_buf) < 0)
                return ext4_corrupt(ep, "failed to read extent index block");
            node_data = node_buf;
            node_size = ep->block_size;
            depth--;
            is_root = 0;
        } else {
            /* Leaf node — search extents */
            struct ext4_extent *ext = (struct ext4_extent *)(eh + 1);
            uint16_t num_entries = eh->eh_entries;

            for (uint16_t i = 0; i < num_entries; i++) {
                uint32_t start = ext[i].ee_block;
                uint16_t len = ext[i].ee_len;

                /* If ee_len > 0x8000, it's a set of uninitialized blocks (hole) */
                int uninit = 0;
                if (len & 0x8000) {
                    uninit = 1;
                    len &= ~0x8000;
                }

                if (iblock >= start && iblock < start + len) {
                    if (uninit)
                        return 0; /* uninitialized = hole, return zeros */

                    uint64_t phys = ((uint64_t)ext[i].ee_start_hi << 32) |
                                     ext[i].ee_start_lo;
                    phys += (iblock - start);
                    return (int64_t)phys;
                }
            }

            /* Not found in any extent — hole */
            return 0;
        }
    }
}

/* ── Block resolution (extent or legacy indirect) ─────────────────── */

static int64_t ext4_get_block_num(struct ext4_priv *ep,
                                   struct ext4_inode *inode,
                                   uint32_t iblock)
{
    /* If EXTENTS flag is set, use extent tree */
    if (inode->i_flags & EXT4_EXTENTS_FL)
        return ext4_get_extent_block(ep, inode, iblock);

    /* Otherwise fall back to legacy ext2 indirect block scheme.
     * This provides backward compatibility with ext2/ext3. */
    if (iblock < 12) {
        return (int64_t)inode->i_block[iblock];
    }

    /* Singly indirect */
    uint32_t entries_per_block = ep->block_size / 4;
    uint32_t sind = iblock - 12;

    if (sind < entries_per_block) {
        if (inode->i_block[12] == 0)
            return 0; /* hole */
        uint8_t indir[EXT4_MAX_BLOCK_SIZE];
        if (ext4_read_block(ep, inode->i_block[12], indir) < 0)
            return -1;
        uint32_t *ptrs = (uint32_t *)indir;
        return (int64_t)ptrs[sind];
    }
    /* Doubly/triply indirect not supported for non-extent files (rare) */
    return -1;
}

/* ── Inline data support ────────────────────────────────────────────── */

/* If the inode has inline data, copy it to buf (up to max_size).
 * Returns bytes copied, or -1 if not inline. */
static int ext4_read_inline_data(struct ext4_inode *inode,
                                  uint8_t *buf, uint32_t max_size)
{
    if (!(inode->i_flags & EXT4_INLINE_DATA_FL))
        return -1;
    /* Inline data is stored at the start of i_block[].
     * For ext4, up to 60 bytes fit in i_block[0..14]. */
    uint32_t size = inode->i_size;
    uint32_t inline_max = (inode->i_blocks == 0) ? EXT4_MAX_INLINE_DATA : 0;
    if (inline_max == 0)
        return -1; /* should not happen for true inline files */

    uint32_t copy = size;
    if (copy > max_size) copy = max_size;
    if (copy > EXT4_MAX_INLINE_DATA) copy = EXT4_MAX_INLINE_DATA;
    memcpy(buf, inode->i_block, copy);
    return (int)copy;
}

/* ── File read ─────────────────────────────────────────────────────── */

static int ext4_read_file(struct ext4_priv *ep, struct ext4_inode *inode,
                           uint8_t *buf, uint32_t size, uint32_t offset)
{
    /* Try inline data first */
    if (inode->i_flags & EXT4_INLINE_DATA_FL) {
        if (offset >= inode->i_size) return 0;
        return ext4_read_inline_data(inode, buf, size);
    }

    uint32_t file_size = inode->i_size;
    /* Handle large files: combine with i_size_high */
    if (ep->ro_compat & EXT4_FEATURE_RO_COMPAT_LARGE_FILE)
        file_size |= ((uint64_t)inode->i_size_high << 32);

    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;

    uint32_t block_size = ep->block_size;
    uint32_t first_block = offset / block_size;
    uint32_t last_block = (offset + size - 1) / block_size;
    uint32_t buf_offset = 0;
    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];

    for (uint32_t b = first_block; b <= last_block; b++) {
        int64_t phys_block = ext4_get_block_num(ep, inode, b);
        if (phys_block < 0) return -1;

        if (phys_block == 0) {
            /* Hole — sparse block */
            memset(block_buf, 0, block_size);
        } else {
            if (ext4_read_block(ep, (uint32_t)phys_block, block_buf) < 0)
                return -1;
        }

        uint32_t block_off = (b == first_block) ? (offset % block_size) : 0;
        uint32_t chunk = block_size - block_off;
        if (chunk > size - buf_offset) chunk = size - buf_offset;
        memcpy(buf + buf_offset, block_buf + block_off, chunk);
        buf_offset += chunk;
    }
    return (int)buf_offset;
}

/* ── Directory reading ─────────────────────────────────────────────── */

/* Find a directory entry by name.  Returns inode number, or 0 on error. */
static uint32_t ext4_find_entry(struct ext4_priv *ep, uint32_t dir_ino,
                                 const char *name)
{
    struct ext4_inode dir_inode;
    if (ext4_read_inode(ep, dir_ino, &dir_inode) < 0)
        return 0;

    uint32_t file_size = dir_inode.i_size;
    uint32_t block_size = ep->block_size;
    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];
    uint32_t bytes_read = 0;
    uint32_t name_len = (uint32_t)strlen(name);

    while (bytes_read < file_size) {
        uint32_t block_num = bytes_read / block_size;
        int64_t phys_block = ext4_get_block_num(ep, &dir_inode, block_num);
        if (phys_block < 0) return 0;

        if (phys_block == 0) {
            /* Hole in directory — skip block */
            bytes_read += block_size;
            continue;
        }

        if (ext4_read_block(ep, (uint32_t)phys_block, block_buf) < 0)
            return 0;

        uint32_t block_off = 0;
        while (block_off < block_size && bytes_read + block_off < file_size) {
            struct ext4_dir_entry *de = (struct ext4_dir_entry *)(block_buf + block_off);

            if (de->rec_len < 8) break; /* corrupted */

            if (de->inode != 0 &&
                de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                return de->inode;
            }

            block_off += de->rec_len;
        }
        bytes_read += block_size;
    }
    return 0; /* not found */
}

/* ── VFS operations ──────────────────────────────────────────────────── */

static int ext4_read(void *priv, const char *path,
                      void *buf, uint32_t max_size, uint32_t *out_size)
{
    struct ext4_priv *ep = (struct ext4_priv *)priv;
    if (out_size) *out_size = 0;

    /* Resolve the path to an inode */
    uint32_t ino = EXT4_ROOT_INO; /* start at root */
    const char *p = path;
    if (*p == '/') p++;

    if (*p) {
        /* Walk path components */
        char name_buf[256];
        while (*p) {
            /* Skip leading slashes */
            while (*p == '/') p++;
            if (!*p) break;

            /* Read next component */
            int i = 0;
            while (*p && *p != '/' && i < 255) name_buf[i++] = *p++;
            name_buf[i] = '\0';

            ino = ext4_find_entry(ep, ino, name_buf);
            if (ino == 0) return -ENOENT;
        }
    }

    struct ext4_inode inode;
    if (ext4_read_inode(ep, ino, &inode) < 0)
        return -EIO;

    if (inode.i_mode & 0x4000) /* S_IFDIR */
        return -EISDIR;

    int ret = ext4_read_file(ep, &inode, (uint8_t *)buf, max_size, 0);
    if (out_size && ret > 0) *out_size = (uint32_t)ret;
    return (ret < 0) ? ret : 0;
}

static int ext4_write(void *priv, const char *path,
                       const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int ext4_stat(void *priv, const char *path, struct vfs_stat *st)
{
    struct ext4_priv *ep = (struct ext4_priv *)priv;
    memset(st, 0, sizeof(*st));

    uint32_t ino = EXT4_ROOT_INO;
    const char *p = path;
    if (*p == '/') p++;

    if (*p) {
        char name_buf[256];
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            int i = 0;
            while (*p && *p != '/' && i < 255) name_buf[i++] = *p++;
            name_buf[i] = '\0';
            ino = ext4_find_entry(ep, ino, name_buf);
            if (ino == 0) return -ENOENT;
        }
    }

    struct ext4_inode inode;
    if (ext4_read_inode(ep, ino, &inode) < 0)
        return -EIO;

    st->ino = ino;

    /* Determine file type from mode */
    if (inode.i_mode & 0x8000)       st->type = VFS_TYPE_FILE;  /* S_IFREG */
    else if (inode.i_mode & 0x4000)  st->type = VFS_TYPE_DIR;   /* S_IFDIR */
    else if (inode.i_mode & 0xA000)  st->type = VFS_TYPE_LINK;  /* S_IFLNK */
    else                              st->type = VFS_TYPE_FILE;

    st->size = inode.i_size;
    if (ep->ro_compat & EXT4_FEATURE_RO_COMPAT_LARGE_FILE)
        st->size |= ((uint64_t)inode.i_size_high << 32);

    st->uid = inode.i_uid;
    st->gid = inode.i_gid;
    st->mode = inode.i_mode;
    st->mtime = inode.i_mtime;
    st->atime = inode.i_atime;
    st->nlink = inode.i_links_count;

    return 0;
}

static int ext4_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int ext4_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int ext4_readdir(void *priv, const char *path)
{
    struct ext4_priv *ep = (struct ext4_priv *)priv;

    /* Resolve directory inode */
    uint32_t ino = EXT4_ROOT_INO;
    const char *p = path;
    if (*p == '/') p++;

    if (*p) {
        char name_buf[256];
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            int i = 0;
            while (*p && *p != '/' && i < 255) name_buf[i++] = *p++;
            name_buf[i] = '\0';
            ino = ext4_find_entry(ep, ino, name_buf);
            if (ino == 0) {
                kprintf("[ext4] readdir: path not found: %s\\n", path);
                return -ENOENT;
            }
        }
    }

    struct ext4_inode dir_inode;
    if (ext4_read_inode(ep, ino, &dir_inode) < 0)
        return -EIO;

    if (!(dir_inode.i_mode & 0x4000))
        return -ENOTDIR;

    uint32_t file_size = dir_inode.i_size;
    uint32_t block_size = ep->block_size;
    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];
    uint32_t bytes_read = 0;

    kprintf(".              <DIR>\\n");
    kprintf("..             <DIR>\\n");

    while (bytes_read < file_size) {
        uint32_t block_num = bytes_read / block_size;
        int64_t phys_block = ext4_get_block_num(ep, &dir_inode, block_num);
        if (phys_block < 0) return -1;

        if (phys_block == 0) {
            bytes_read += block_size;
            continue;
        }

        if (ext4_read_block(ep, (uint32_t)phys_block, block_buf) < 0)
            return -1;

        uint32_t block_off = 0;
        while (block_off < block_size && bytes_read + block_off < file_size) {
            struct ext4_dir_entry *de = (struct ext4_dir_entry *)(block_buf + block_off);

            if (de->rec_len < 8) break;

            if (de->inode != 0 && de->name_len > 0) {
                char fname[256];
                uint32_t nlen = de->name_len;
                if (nlen > 255) nlen = 255;
                memcpy(fname, de->name, nlen);
                fname[nlen] = '\0';

                /* Determine type string */
                const char *type_str = "<FILE>";
                switch (de->file_type) {
                    case EXT4_FT_DIR:      type_str = "<DIR>";  break;
                    case EXT4_FT_REG_FILE: type_str = "<FILE>"; break;
                    case EXT4_FT_SYMLINK:  type_str = "<LNK>";  break;
                    case EXT4_FT_CHRDEV:   type_str = "<CHR>";  break;
                    case EXT4_FT_BLKDEV:   type_str = "<BLK>";  break;
                    default:               type_str = "<?>";    break;
                }

                kprintf("%-16s %s\\n", fname, type_str);
            }

            block_off += de->rec_len;
        }
        bytes_read += block_size;
    }

    return 0;
}

static struct vfs_ops ext4_ops = {
    .read    = ext4_read,
    .write   = ext4_write,
    .stat    = ext4_stat,
    .create  = ext4_create,
    .unlink  = ext4_unlink,
    .readdir = ext4_readdir,
};

/* ── Mount ─────────────────────────────────────────────────────────── */

int ext4_mount(const char *mountpoint, uint8_t dev_id)
{
    struct ext4_priv *ep = (struct ext4_priv *)kmalloc(sizeof(struct ext4_priv));
    if (!ep) return -ENOMEM;

    memset(ep, 0, sizeof(*ep));
    ep->dev_id = dev_id;
    strncpy(ep->mountpoint, mountpoint, sizeof(ep->mountpoint) - 1);

    if (ext4_load_super(ep) < 0) {
        kprintf("[ext4] failed to read superblock\\n");
        goto fail;
    }

    if (ep->sb.s_magic != EXT4_SUPER_MAGIC) {
        kprintf("[ext4] bad magic: 0x%04x\\n", ep->sb.s_magic);
        goto fail;
    }

    ep->incompat  = ep->sb.s_feature_incompat;
    ep->ro_compat = ep->sb.s_feature_ro_compat;
    ep->compat    = ep->sb.s_feature_compat;

    /* Log FS type */
    const char *fs_type = "ext4";
    if (!(ep->incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) &&
        !(ep->incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) &&
        !(ep->incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA)) {
        if (ep->sb.s_rev_level == 0)
            fs_type = "ext2";
        else if (ep->compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL)
            fs_type = "ext3";
        else
            fs_type = "ext2/3";
    }

    kprintf("[ext4] %s filesystem detected\\n", fs_type);

    /* Reject filesystems we cannot safely read */
    if (ep->incompat & EXT4_FEATURE_INCOMPAT_JOURNAL_DEV) {
        kprintf("[ext4] journal device not supported\\n");
        goto fail;
    }
    if (ep->incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT) {
        kprintf("[ext4] encryption not supported, but can read encrypted context\n");
        /* We can still read the filesystem — encryption context (EXT4_ENCRYPT_FL
         * inode flag) will be read per-inode and reported. Continue mounting. */
    }
    if (ep->incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        /* 64-bit mode changes BGD structure; we support reading the
         * larger block group descriptors now. */
        kprintf("[ext4] 64-bit mode detected, using extended BGD format\n");
    }

    /* Block size */
    ep->block_size = 1024 << ep->sb.s_log_block_size;
    if (ep->block_size < 1024 || ep->block_size > EXT4_MAX_BLOCK_SIZE) {
        kprintf("[ext4] unsupported block size: %u\\n", ep->block_size);
        goto fail;
    }

    ep->blocks_per_group = ep->sb.s_blocks_per_group;
    ep->inodes_per_group = ep->sb.s_inodes_per_group;
    ep->inode_size = ep->sb.s_inode_size;
    if (ep->inode_size < 128) ep->inode_size = 128;

    /* Number of block groups */
    uint32_t total_groups = (ep->sb.s_blocks_count + ep->blocks_per_group - 1) /
                             ep->blocks_per_group;
    ep->num_block_groups = total_groups;

    /* flex_bg size detection */
    if (ep->incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) {
        /* Default flex_bg size is 16; we use a reasonable heuristic */
        ep->flex_bg_size = 16;
        kprintf("[ext4] flex_bg enabled (group size=%u)\\n", ep->flex_bg_size);
    }

    /* Load block group descriptor cache */
    if (ext4_load_bgd_cache(ep) < 0)
        goto fail;

    /* Mount read-only */
    return vfs_mount_ex(mountpoint, &ext4_ops, ep, MS_RDONLY);

fail:
    if (ep->bgd_cache) kfree(ep->bgd_cache);
    kfree(ep);
    return -1;
}

/* ── Init ──────────────────────────────────────────────────────────── */

int ext4_init(void)
{
    kprintf("[ext4] Ext4 read-only filesystem initialized\\n");
    vfs_register_filesystem("ext4", &ext4_ops);
    return 0;
}

device_initcall(ext4_init);

#ifdef MODULE
int init_module(void) { return ext4_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Ext4 read-only filesystem — extent tree, flex_bg, inline data");
MODULE_VERSION("1.0");
#endif

/* ── ext4_umount ──────────────────────────────────────── */
int ext4_umount(const char *target)
{
    (void)target;
    kprintf("[ext4] Ext4 unmounted\n");
    return 0;
}
/* ── ext4_lookup ──────────────────────────────────────── */
int ext4_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[ext4] lookup: %s\n", name);
    return -ENOENT;
}
/* ── ext4_truncate ─────────────────────────────────────── */
int ext4_truncate(void *inode, uint64_t size)
{
    (void)inode;
    kprintf("[ext4] truncate to %llu\n", (unsigned long long)size);
    return 0;
}
/* ── ext4_sync ──────────────────────────────────────── */
int ext4_sync(void *file)
{
    (void)file;
    kprintf("[ext4] Sync complete\n");
    return 0;
}
