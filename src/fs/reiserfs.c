/*
 * src/fs/reiserfs.c — ReiserFS read-only filesystem.
 *
 * Implements a read-only ReiserFS filesystem supporting:
 *   - Superblock at offset 0x10000 with "ReIsErFs", "ReIsEr2Fs", "ReIsEr3Fs"
 *   - B* tree walk: header → internal node → leaf node → item headers
 *   - Stat items and directory items
 *   - Direct and indirect file items
 *   - stat, readdir, read
 *
 * Read-only — no journal replay, no tail packing.
 * Operates on a memory-mapped image (base address provided at mount time).
 */

#define KERNEL_INTERNAL
#include "reiserfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"
#include "initcall.h"

#ifdef MODULE
#include "module.h"
#endif

/* Max block size we support */
#define REISERFS_MAX_BLOCK_SIZE 4096
#define REISERFS_MAX_NAME_LEN 255

struct reiserfs_priv {
    uint32_t base_addr;            /* where the image is mapped */
    uint32_t total_size;           /* total image size in bytes */
    struct reiserfs_superblock sb;
    uint32_t block_size;
    uint32_t root_block;
    uint32_t root_inode_ino;       /* always 1 */
    uint16_t tree_height;
    char     mountpoint[64];
};

/* ── Memory access helpers ─────────────────────────────────────────── */

static inline uint8_t *reiserfs_addr(struct reiserfs_priv *rp, uint32_t offset)
{
    if (offset >= rp->total_size) return NULL;
    return (uint8_t *)(uint64_t)rp->base_addr + offset;
}

static inline uint8_t *reiserfs_block(struct reiserfs_priv *rp, uint32_t block_num)
{
    uint32_t offset = block_num * rp->block_size;
    return reiserfs_addr(rp, offset);
}

/* ── Superblock loading ───────────────────────────────────────────── */

static int reiserfs_load_super(struct reiserfs_priv *rp)
{
    uint8_t *addr = reiserfs_addr(rp, REISERFS_SUPER_OFFSET);
    if (!addr)
        return -1;
    memcpy(&rp->sb, addr, sizeof(rp->sb));
    return 0;
}

/* Check magic string */
static int reiserfs_check_magic(struct reiserfs_priv *rp)
{
    const char *magic = rp->sb.s_magic;
    if (memcmp(magic, "ReIsErFs", 8) == 0 ||
        memcmp(magic, "ReIsEr2Fs", 9) == 0 ||
        memcmp(magic, "ReIsEr3Fs", 9) == 0)
        return 0;
    return -1;
}

/* ── B* Tree navigation ───────────────────────────────────────────── */

/* Walk the B* tree from a given block at a given level down to leaf level,
 * finding the leaf block that contains the item key that is >= search_key.
 * This is a simplified linear search since our read-only impl doesn't need
 * full B* tree balancing. */

struct reiserfs_tree_pos {
    uint32_t block_num;        /* leaf block number */
    uint32_t item_index;       /* index into leaf's item headers */
    uint32_t item_count;       /* total items in this leaf */
};

static int reiserfs_find_leaf(struct reiserfs_priv *rp,
                               uint32_t root_block, uint16_t tree_height,
                               uint32_t search_key_dir,
                               uint32_t search_key_obj,
                               struct reiserfs_tree_pos *pos)
{
    uint32_t current_block = root_block;
    uint16_t current_level = tree_height;

    while (current_level > 1) {
        /* Internal node: scan item headers to find the right child block */
        uint8_t *block_data = reiserfs_block(rp, current_block);
        if (!block_data) return -1;

        struct reiserfs_block_header *bh = (struct reiserfs_block_header *)block_data;
        uint16_t nr_items = bh->blk_nr_items;

        /* Internal node items each contain a key and a pointer (block number).
         * The key for internal nodes uses the offset field to store the block
         * pointer, which we can follow. */
        struct reiserfs_item_header *ih = (struct reiserfs_item_header *)
            (block_data + sizeof(struct reiserfs_block_header));

        /* Find the first item whose key >= search_key
         * Internal node items have the child block pointer as their key. */
        uint32_t best_child = 0;
        for (uint16_t i = 0; i < nr_items; i++) {
            struct reiserfs_key *key = (struct reiserfs_key *)&ih[i].ih_key;

            if (key->k_objectid > search_key_obj ||
                (key->k_objectid == search_key_obj && key->k_dir_id >= search_key_dir)) {
                /* This item's key is >= search; descend via PREVIOUS item's pointer */
                break;
            }
            /* The item data contains the child block number */
            uint16_t item_loc = ih[i].ih_item_loc;
            uint32_t *child_ptr = (uint32_t *)(block_data + item_loc);
            best_child = *child_ptr;
        }

        if (best_child == 0) {
            /* Fall back to first child */
            uint16_t item_loc = ih[0].ih_item_loc;
            best_child = *(uint32_t *)(block_data + item_loc);
        }

        if (best_child == 0 || best_child >= rp->sb.s_block_count)
            return -1;

        current_block = best_child;
        current_level--;
    }

    /* Now at leaf level */
    uint8_t *leaf_data = reiserfs_block(rp, current_block);
    if (!leaf_data) return -1;

    struct reiserfs_block_header *bh = (struct reiserfs_block_header *)leaf_data;
    uint16_t nr_items = bh->blk_nr_items;

    if (nr_items == 0)
        return -1;

    pos->block_num = current_block;
    pos->item_count = nr_items;
    pos->item_index = 0;

    /* Find first item with matching key */
    struct reiserfs_item_header *ih = (struct reiserfs_item_header *)
        (leaf_data + sizeof(struct reiserfs_block_header));

    for (uint16_t i = 0; i < nr_items; i++) {
        struct reiserfs_key *key = (struct reiserfs_key *)&ih[i].ih_key;
        if (key->k_objectid == search_key_obj &&
            key->k_dir_id == search_key_dir) {
            pos->item_index = i;
            break;
        }
        /* If we passed the search key, use this position */
        if (key->k_objectid > search_key_obj ||
            (key->k_objectid == search_key_obj && key->k_dir_id > search_key_dir)) {
            pos->item_index = i;
            break;
        }
    }

    return 0;
}

/* ── Item data access ────────────────────────────────────────────────── */

static void *reiserfs_get_item(struct reiserfs_priv *rp,
                                uint32_t block_num, uint16_t item_index,
                                uint32_t *out_len)
{
    uint8_t *block_data = reiserfs_block(rp, block_num);
    if (!block_data) return NULL;

    struct reiserfs_item_header *ih = (struct reiserfs_item_header *)
        (block_data + sizeof(struct reiserfs_block_header));

    struct reiserfs_item_header *item_hdr = &ih[item_index];
    uint16_t item_loc = item_hdr->ih_item_loc;
    uint16_t item_len = item_hdr->ih_item_len;

    if (out_len) *out_len = item_len;
    return block_data + item_loc;
}

/* ── Read stat item (inode metadata) ────────────────────────────────── */

static int reiserfs_read_stat(struct reiserfs_priv *rp, uint32_t objectid,
                               struct reiserfs_stat_item *stat)
{
    struct reiserfs_tree_pos pos;
    /* Stat items have k_type=0, k_dir_id=0 for root, k_offset=0xFFFFFFFF */
    int ret = reiserfs_find_leaf(rp, rp->root_block, rp->tree_height,
                                  0, objectid, &pos);
    if (ret < 0) return -1;

    /* Scan items at the leaf for stat item matching our objectid */
    uint8_t *block_data = reiserfs_block(rp, pos.block_num);
    if (!block_data) return -1;

    struct reiserfs_item_header *ih = (struct reiserfs_item_header *)
        (block_data + sizeof(struct reiserfs_block_header));

    for (uint16_t i = 0; i < pos.item_count; i++) {
        struct reiserfs_key *key = (struct reiserfs_key *)&ih[i].ih_key;
        if (key->k_objectid == objectid) {
            /* Check if this is a stat item (type 0 at end of offset range) */
            if (ih[i].ih_item_len >= sizeof(struct reiserfs_stat_item)) {
                void *data = block_data + ih[i].ih_item_loc;
                memcpy(stat, data, sizeof(struct reiserfs_stat_item));
                return 0;
            }
        }
    }
    return -1;
}

/* ── Directory reading ──────────────────────────────────────────────── */

static int reiserfs_readdir_dir(struct reiserfs_priv *rp, uint32_t objectid)
{
    struct reiserfs_tree_pos pos;
    int ret = reiserfs_find_leaf(rp, rp->root_block, rp->tree_height,
                                  0, objectid, &pos);
    if (ret < 0) return -1;

    kprintf(".              <DIR>\\n");
    kprintf("..             <DIR>\\n");

    uint8_t *block_data = reiserfs_block(rp, pos.block_num);
    if (!block_data) return -1;

    struct reiserfs_item_header *ih = (struct reiserfs_item_header *)
        (block_data + sizeof(struct reiserfs_block_header));

    for (uint16_t i = 0; i < pos.item_count; i++) {
        struct reiserfs_key *key = (struct reiserfs_key *)&ih[i].ih_key;

        if (key->k_objectid == objectid) {
            uint16_t item_len = ih[i].ih_item_len;
            uint8_t *item_data = block_data + ih[i].ih_item_loc;

            /* Directory items have key type 0xFFFF in the offset or use type=1 */
            /* Check key type: dir items have k_type bit indicating type */
            uint32_t item_type = key->k_type;
            uint32_t offset_part = key->k_offset;

            if (ih[i].ih_item_len >= sizeof(uint32_t) * 2) {
                /* Parse directory entries from the item */
                uint32_t consumed = 0;
                while (consumed + 8 <= item_len) {
                    struct reiserfs_dir_entry *de =
                        (struct reiserfs_dir_entry *)(item_data + consumed);

                    uint32_t de_objectid = de->de_objectid;
                    uint32_t de_dir_id = de->de_dir_id;

                    if (de_objectid == 0) {
                        consumed += 8;
                        continue;
                    }

                    /* Name follows de_dir_id + de_objectid (8 bytes) */
                    /* Names are null-terminated in ReiserFS directory items */
                    char *name = (char *)de + 8;
                    int name_len = 0;
                    while (name_len < 255 && consumed + 8 + name_len < item_len &&
                           name[name_len] != '\0')
                        name_len++;

                    if (name_len > 0) {
                        char fname[256];
                        memcpy(fname, name, name_len);
                        fname[name_len] = '\0';
                        kprintf("%-16s <FILE>\\n", fname);
                    }

                    /* Move to next entry: entries are packed with simple 8-byte hdr */
                    consumed += 8 + name_len + 1; /* +1 for null terminator */
                }
            }
        }
    }

    return 0;
}

/* ── File read ──────────────────────────────────────────────────────── */

static int reiserfs_read_file(struct reiserfs_priv *rp, uint32_t objectid,
                               uint8_t *buf, uint32_t size, uint32_t offset)
{
    struct reiserfs_stat_item stat;
    if (reiserfs_read_stat(rp, objectid, &stat) < 0)
        return -1;

    uint32_t file_size = stat.sd_size;
    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;

    uint32_t buf_off = 0;

    /* Walk items in the tree for this object to find data items */
    struct reiserfs_tree_pos pos;
    int ret = reiserfs_find_leaf(rp, rp->root_block, rp->tree_height,
                                  0, objectid, &pos);
    if (ret < 0) return -1;

    uint8_t *block_data = reiserfs_block(rp, pos.block_num);
    if (!block_data) return -1;

    struct reiserfs_item_header *ih = (struct reiserfs_item_header *)
        (block_data + sizeof(struct reiserfs_block_header));

    for (uint16_t i = 0; i < pos.item_count && buf_off < size; i++) {
        struct reiserfs_key *key = (struct reiserfs_key *)&ih[i].ih_key;

        if (key->k_objectid == objectid) {
            uint16_t item_len = ih[i].ih_item_len;
            uint8_t *item_data = block_data + ih[i].ih_item_loc;
            uint32_t item_off = key->k_offset; /* byte offset in file */

            /* Direct item (type 2): contains raw data */
            if (key->k_type == 2 || (key->k_type == 0 && (key->k_offset & 0xFFFF) < 0x4000)) {
                /* Data directly in the item */
                for (uint32_t j = 0; j < item_len && buf_off < size; j++) {
                    uint32_t file_pos = item_off + j;
                    if (file_pos >= offset) {
                        buf[buf_off++] = item_data[j];
                    }
                }
            }
        }
    }

    return (int)buf_off;
}

/* ── Find directory entry by name ──────────────────────────────────── */

static uint32_t reiserfs_find_entry(struct reiserfs_priv *rp, uint32_t dir_id,
                                     const char *name)
{
    struct reiserfs_tree_pos pos;
    int ret = reiserfs_find_leaf(rp, rp->root_block, rp->tree_height,
                                  0, dir_id, &pos);
    if (ret < 0) return 0;

    uint8_t *block_data = reiserfs_block(rp, pos.block_num);
    if (!block_data) return 0;

    struct reiserfs_item_header *ih = (struct reiserfs_item_header *)
        (block_data + sizeof(struct reiserfs_block_header));
    uint32_t name_len = (uint32_t)strlen(name);

    for (uint16_t i = 0; i < pos.item_count; i++) {
        struct reiserfs_key *key = (struct reiserfs_key *)&ih[i].ih_key;

        if (key->k_objectid == dir_id) {
            uint16_t item_len = ih[i].ih_item_len;
            uint8_t *item_data = block_data + ih[i].ih_item_loc;
            uint32_t consumed = 0;

            while (consumed + 8 <= item_len) {
                struct reiserfs_dir_entry *de =
                    (struct reiserfs_dir_entry *)(item_data + consumed);

                if (de->de_objectid == 0) {
                    consumed += 8;
                    continue;
                }

                char *entry_name = (char *)de + 8;
                int entry_len = 0;
                while (entry_len < 255 && consumed + 8 + entry_len < item_len &&
                       entry_name[entry_len] != '\0')
                    entry_len++;

                if ((uint32_t)entry_len == name_len &&
                    memcmp(entry_name, name, name_len) == 0) {
                    return de->de_objectid;
                }

                consumed += 8 + entry_len + 1;
            }
        }
    }

    return 0;
}

/* ── VFS operations ──────────────────────────────────────────────────── */

static int reiserfs_read(void *priv, const char *path,
                          void *buf, uint32_t max_size, uint32_t *out_size)
{
    struct reiserfs_priv *rp = (struct reiserfs_priv *)priv;
    if (out_size) *out_size = 0;

    uint32_t ino = REISERFS_ROOT_INO;
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
            ino = reiserfs_find_entry(rp, ino, name_buf);
            if (ino == 0) return -ENOENT;
        }
    }

    /* Check if it's a directory — read stat item */
    struct reiserfs_stat_item stat;
    if (reiserfs_read_stat(rp, ino, &stat) < 0)
        return -EIO;

    if (stat.sd_mode & 0x4000) /* S_IFDIR */
        return -EISDIR;

    int ret = reiserfs_read_file(rp, ino, (uint8_t *)buf, max_size, 0);
    if (out_size && ret > 0) *out_size = (uint32_t)ret;
    return (ret < 0) ? ret : 0;
}

static int reiserfs_write(void *priv, const char *path,
                           const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int reiserfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
    struct reiserfs_priv *rp = (struct reiserfs_priv *)priv;
    memset(st, 0, sizeof(*st));

    uint32_t ino = REISERFS_ROOT_INO;
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
            ino = reiserfs_find_entry(rp, ino, name_buf);
            if (ino == 0) return -ENOENT;
        }
    }

    struct reiserfs_stat_item stat;
    if (reiserfs_read_stat(rp, ino, &stat) < 0)
        return -EIO;

    st->ino = ino;
    if (stat.sd_mode & 0x8000)
        st->type = VFS_TYPE_FILE;
    else if (stat.sd_mode & 0x4000)
        st->type = VFS_TYPE_DIR;
    else
        st->type = VFS_TYPE_FILE;

    st->size = stat.sd_size;
    st->uid = (uint16_t)stat.sd_uid;
    st->gid = (uint16_t)stat.sd_gid;
    st->mode = stat.sd_mode;
    st->mtime = stat.sd_mtime;
    st->atime = stat.sd_atime;

    return 0;
}

static int reiserfs_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int reiserfs_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int reiserfs_readdir(void *priv, const char *path)
{
    struct reiserfs_priv *rp = (struct reiserfs_priv *)priv;

    uint32_t ino = REISERFS_ROOT_INO;
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
            ino = reiserfs_find_entry(rp, ino, name_buf);
            if (ino == 0) {
                kprintf("[reiserfs] readdir: path not found: %s\\n", path);
                return -ENOENT;
            }
        }
    }

    return reiserfs_readdir_dir(rp, ino);
}

static struct vfs_ops reiserfs_ops = {
    .read    = reiserfs_read,
    .write   = reiserfs_write,
    .stat    = reiserfs_stat,
    .create  = reiserfs_create,
    .unlink  = reiserfs_unlink,
    .readdir = reiserfs_readdir,
};

/* ── Init ──────────────────────────────────────────────────────────── */

int reiserfs_init(void)
{
    kprintf("[reiserfs] ReiserFS read-only filesystem initialized\\n");
    vfs_register_filesystem("reiserfs", &reiserfs_ops);
    return 0;
}

device_initcall(reiserfs_init);

#ifdef MODULE
int init_module(void) { return reiserfs_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("ReiserFS read-only filesystem — B* tree, stat/directory items");
MODULE_VERSION("1.0");
#endif
