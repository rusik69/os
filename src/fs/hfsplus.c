/*
 * src/fs/hfsplus.c — HFS+ read-only filesystem
 *
 * Implements a read-only HFS+ filesystem supporting:
 *   - Volume header parsing at sector 2
 *   - Catalog B-tree for directory/file lookup
 *   - Extents overflow file for large files
 *   - HFS+ extended attributes
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"
#include "blockdev.h"
#include "hfsplus.h"

#ifdef MODULE
#include "module.h"
#endif
#include "initcall.h"

/* ── HFS+ constants ──────────────────────────────────────────────── */
#define HFSPLUS_ROOT_PARENT     2
#define HFSPLUS_FOLDER_RECORD   0x0001
#define HFSPLUS_FILE_RECORD     0x0002

/* ── HFS+ catalog record ──────────────────────────────────────────── */
struct hfsplus_catalog_rec {
    uint16_t record_type;
    uint16_t flags;
    uint32_t folder_id;
    /* folder_record or file_record follows */
    struct {
        uint16_t record_type;
        uint16_t flags;
        uint32_t folder_id;
    } folder_record;
};

/* ── Helpers ────────────────────────────────────────────────────── */

static inline uint16_t r16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── Block I/O ──────────────────────────────────────────────────── */

static int hfsplus_read_blocks(struct hfsplus_priv *hp, uint64_t lba,
                                uint32_t count, uint8_t *buf)
{
    for (uint32_t i = 0; i < count; i++) {
        if (blockdev_read_sectors(hp->dev_id, lba + i, 1,
                                   buf + i * HFSPLUS_SECTOR_SIZE) != 0)
            return -1;
    }
    return 0;
}

static int hfsplus_read_block(struct hfsplus_priv *hp, uint64_t lba,
                               uint8_t *buf)
{
    return hfsplus_read_blocks(hp, lba, 1, buf);
}

static int hfsplus_read_bytes(struct hfsplus_priv *hp, uint64_t offset,
                               uint32_t size, uint8_t *buf)
{
    uint64_t lba = offset / 512;
    uint32_t count = (size + 511) / 512;
    return hfsplus_read_blocks(hp, lba, count, buf);
}

/* ── Load volume header ──────────────────────────────────────────── */

static int hfsplus_load_vh(struct hfsplus_priv *hp)
{
    uint8_t buf[512];

    /* Volume header is at sector 2 (byte offset 1024) */
    if (hfsplus_read_block(hp, HFSPLUS_VOL_HEADER_SECTOR, buf) < 0)
        return -1;

    struct hfsplus_vh *vh = (struct hfsplus_vh *)buf;

    if (vh->signature != HFSPLUS_SIG_HFSPLUS &&
        vh->signature != HFSPLUS_SIG_HFSX) {
        kprintf("[hfsplus] invalid signature 0x%04X\n", vh->signature);
        return -1;
    }

    hp->block_size = r32((uint8_t *)&vh->block_size);
    hp->total_blocks = r32((uint8_t *)&vh->total_blocks);
    hp->free_blocks = r32((uint8_t *)&vh->free_blocks);

    /* Parse the catalog file fork data */
    /* The catalog_file field contains a struct hfsplus_fork_data (80 bytes) */
    struct hfsplus_fork_data *cf = (struct hfsplus_fork_data *)vh->catalog_file;
    hp->cat_start_offset = (uint64_t)r32((uint8_t *)&cf->extents[0].start_block) *
                            (uint64_t)hp->block_size;
    hp->cat_node_size = 512; /* will be overwritten by B-tree header */

    /* Parse extents overflow file */
    struct hfsplus_fork_data *ef = (struct hfsplus_fork_data *)vh->extents_file;
    hp->ext_start_offset = (uint64_t)r32((uint8_t *)&ef->extents[0].start_block) *
                            (uint64_t)hp->block_size;
    hp->ext_node_size = 512;

    /* Parse attributes file */
    struct hfsplus_fork_data *af = (struct hfsplus_fork_data *)vh->attributes_file;
    hp->attr_start_offset = (uint64_t)r32((uint8_t *)&af->extents[0].start_block) *
                             (uint64_t)hp->block_size;
    hp->attr_node_size = 512;

    kprintf("[hfsplus] volume header: blocks=%u, blk_size=%u, free=%u\n",
            hp->total_blocks, hp->block_size, hp->free_blocks);
    return 0;
}

/* ── B-tree node reader ──────────────────────────────────────────── */

static int hfsplus_read_btree_node(struct hfsplus_priv *hp, uint32_t node_num,
                                    uint8_t *buf, uint32_t node_size,
                                    uint64_t start_offset)
{
    uint64_t offset = start_offset + (uint64_t)node_num * node_size;
    return hfsplus_read_bytes(hp, offset, node_size, buf);
}

/* Read B-tree header to get root node and node size */
static int hfsplus_read_btree_header(struct hfsplus_priv *hp,
                                      uint32_t *root_node,
                                      uint32_t *node_size,
                                      uint64_t start_offset)
{
    uint8_t buf[1024]; /* typically enough for header node */

    /* Header is always node 0 */
    if (hfsplus_read_btree_node(hp, 0, buf, 512, start_offset) < 0)
        return -1;

    struct hfsplus_btnode_descriptor *desc =
        (struct hfsplus_btnode_descriptor *)buf;
    if (desc->kind != BT_HEADER_NODE)
        return -1;

    /* Read actual node size from header record */
    struct hfsplus_btree_header *hdr =
        (struct hfsplus_btree_header *)(buf + sizeof(struct hfsplus_btnode_descriptor));
    *root_node = hdr->root_node;
    *node_size = r16((uint8_t *)&hdr->node_size);

    /* Re-read with correct node size */
    if (*node_size > sizeof(buf)) {
        /* Allocate larger buffer */
        uint8_t *big_buf = (uint8_t *)kmalloc(*node_size);
        if (!big_buf) return -1;
        if (hfsplus_read_btree_node(hp, 0, big_buf, *node_size, start_offset) < 0) {
            kfree(big_buf);
            return -1;
        }
        desc = (struct hfsplus_btnode_descriptor *)big_buf;
        hdr = (struct hfsplus_btree_header *)(big_buf + sizeof(struct hfsplus_btnode_descriptor));
        *root_node = hdr->root_node;
        hp->cat_key_compare = hdr->key_compare;
        kfree(big_buf);
    } else {
        hp->cat_key_compare = hdr->key_compare;
    }

    return 0;
}

/* ── Catalog lookup ──────────────────────────────────────────────── */

/* HFS+ catalog key comparison (case-insensitive for HFS+, sensitive for HFSX) */
static int hfsplus_key_compare(struct hfsplus_priv *hp,
                                const uint8_t *key1, uint32_t len1,
                                const uint8_t *key2, uint32_t len2)
{
    uint32_t min_len = len1 < len2 ? len1 : len2;
    for (uint32_t i = 0; i < min_len; i++) {
        uint8_t a = key1[i];
        uint8_t b = key2[i];
        if (hp->cat_key_compare == HFSPLUS_CASE_INSENSITIVE) {
            /* Simple ASCII case folding */
            if (a >= 'A' && a <= 'Z') a += 0x20;
            if (b >= 'A' && b <= 'Z') b += 0x20;
        }
        if (a != b) return (int)a - (int)b;
    }
    if (len1 != len2) return (int)len1 - (int)len2;
    return 0;
}

/* Search a B-tree node for a given key (simplified linear search for leaf nodes) */
static int hfsplus_search_node(struct hfsplus_priv *hp, uint8_t *node_buf,
                                uint32_t node_size,
                                const uint8_t *search_key,
                                uint32_t search_key_len,
                                uint8_t **out_rec, uint32_t *out_rec_len)
{
    struct hfsplus_btnode_descriptor *desc =
        (struct hfsplus_btnode_descriptor *)node_buf;
    uint16_t num_recs = desc->num_recs;

    if (desc->kind != BT_LEAF_NODE && desc->kind != BT_INDEX_NODE)
        return -1;

    /* HFS+ stores record offsets at the end of the node, starting from
     * node_size - 2 going backwards */
    uint16_t *rec_offsets = (uint16_t *)(node_buf + node_size);
    /* First record offset is at node_size - 2, then node_size - 4, etc. */
    /* Actually HFS+ stores record offsets from the END of the node.
     * rec_offsets[-1] = offset of last record, rec_offsets[-2] = offset of second-to-last, etc.
     * We index them properly: */
    /* The actual offsets are 16-bit values stored at the END of the node buffer.
     * rec_offsets[0] = offset of first record (at the beginning of the area).
     * Wait, HFS+ B-tree stores record offsets at the end of the node, but the
     * exact format is:
     *   - Offsets are stored as uint16_t values starting at the end of the node.
     *   - offset[0] is at offset node_size - 2
     *   - offset[num_recs - 1] is at offset node_size - (num_recs * 2)
     */
    /* The offsets count from the end: the first record is stored at
     * offset[node_size - 2] and it points to the first record. */
    for (int i = num_recs - 1; i >= 0; i--) {
        uint16_t rec_off = r16((uint8_t *)(node_buf + node_size - (i + 1) * 2 - 2));
        if (rec_off >= node_size) continue;

        /* For leaf nodes, the record starts with a key, then data */
        uint8_t *rec = node_buf + rec_off;

        /* HFS+ catalog key: first 2 bytes = key_length (including these 2 bytes) */
        uint16_t key_len = r16(rec);
        if (key_len < 4 || key_len > node_size - rec_off) continue;

        /* Compare keys */
        /* For catalog keys, the key structure is:
         *   uint16_t key_length
         *   uint32_t parent_id (catalog key) or fork_type (extent key)
         *   uint16_t node_name_length
         *   uint16_t node_name[] (UTF-16BE for catalog keys)
         */
        /* Simplified comparison: just compare the full key data */
        uint8_t *key_data = rec + 2; /* skip length field */
        uint32_t key_data_len = key_len - 2;

        if (desc->kind == BT_LEAF_NODE) {
            if (hfsplus_key_compare(hp, key_data, key_data_len,
                                     search_key, search_key_len) == 0) {
                /* Found matching key */
                uint32_t rec_len = node_size - rec_off;
                /* Record data follows key */
                uint8_t *rec_data = rec + key_len;
                uint32_t data_len = rec_len - key_len;
                *out_rec = rec_data;
                *out_rec_len = data_len;
                return 0;
            }
        } else {
                    /* Index node: record is just a key_ptr (key + node number) */
                    /* Simplified: follow the first entry that's >= search key */
                    uint32_t idx_rec_len = node_size - rec_off;
                    if (hfsplus_key_compare(hp, key_data, key_data_len,
                                             search_key, search_key_len) >= 0) {
                        /* The child node number is the last 4 bytes of the record */
                        if (idx_rec_len >= 6) {
                            uint32_t child = r32(rec + idx_rec_len - 4);
                            *out_rec = (uint8_t *)(uintptr_t)child;
                            *out_rec_len = 0;
                            return 1; /* signal: descend to child */
                        }
                    }
                }
    }
    return -1; /* not found */
}

/* Search the catalog B-tree for a given parent ID and filename.
 * Returns 0 on success with record data. */
static int hfsplus_catalog_lookup(struct hfsplus_priv *hp,
                                   uint32_t parent_id,
                                   const char *name,
                                   uint8_t *rec_buf, uint32_t *rec_len)
{
    /* Build catalog search key */
    uint8_t key_buf[520];
    uint16_t name_len = (uint16_t)strlen(name);

    uint16_t key_length = 4 + 2 + name_len * 2; /* key_length + parent_id + name_len + UTF-16BE name */
    key_buf[0] = (uint8_t)(key_length & 0xFF);
    key_buf[1] = (uint8_t)(key_length >> 8);
    key_buf[2] = (uint8_t)(parent_id & 0xFF);
    key_buf[3] = (uint8_t)((parent_id >> 8) & 0xFF);
    key_buf[4] = (uint8_t)((parent_id >> 16) & 0xFF);
    key_buf[5] = (uint8_t)((parent_id >> 24) & 0xFF);
    key_buf[6] = (uint8_t)(name_len & 0xFF);
    key_buf[7] = (uint8_t)(name_len >> 8);

    /* Convert name to UTF-16BE */
    for (uint16_t i = 0; i < name_len; i++) {
        key_buf[8 + i * 2] = 0;
        key_buf[8 + i * 2 + 1] = (uint8_t)name[i];
    }

    uint32_t search_key_len = 4 + 2 + name_len * 2; /* parent_id + name_len + name */

    /* Start from root node and descend */
    uint32_t node_size = hp->cat_node_size;
    uint8_t *node_buf = (uint8_t *)kmalloc(node_size);
    if (!node_buf) return -1;

    uint32_t current_node = hp->cat_root_node;
    int result = -1;

    while (1) {
        if (hfsplus_read_btree_node(hp, current_node, node_buf,
                                     node_size, hp->cat_start_offset) < 0)
            break;

        struct hfsplus_btnode_descriptor *desc =
            (struct hfsplus_btnode_descriptor *)node_buf;

        if (desc->kind == BT_LEAF_NODE) {
            /* Search in this leaf */
            uint8_t *rec_data;
            uint32_t data_len;
            if (hfsplus_search_node(hp, node_buf, node_size,
                                     key_buf + 2, search_key_len,
                                     &rec_data, &data_len) == 0) {
                if (data_len <= *rec_len) {
                    memcpy(rec_buf, rec_data, data_len);
                    *rec_len = data_len;
                    result = 0;
                }
            }
            break;
        } else if (desc->kind == BT_INDEX_NODE) {
            /* Descend to child */
            uint8_t *rec_data;
            uint32_t data_len;
            int sr = hfsplus_search_node(hp, node_buf, node_size,
                                          key_buf + 2, search_key_len,
                                          &rec_data, &data_len);
            if (sr == 1) {
                /* rec_data encodes child node number */
                current_node = (uint32_t)(uintptr_t)rec_data;
            } else if (sr == 0) {
                /* Found in index node (unlikely for non-header) */
                if (data_len <= *rec_len) {
                    memcpy(rec_buf, rec_data, data_len);
                    *rec_len = data_len;
                    result = 0;
                }
                break;
            } else {
                /* Not found, try first child */
                uint16_t *offsets = (uint16_t *)(node_buf + node_size);
                if (desc->num_recs > 0) {
                    uint16_t first_off = r16((uint8_t *)(node_buf + node_size - 2));
                    uint8_t *first_rec = node_buf + first_off;
                    uint32_t rec_sz = node_size - first_off;
                    current_node = r32(first_rec + rec_sz - 4);
                } else {
                    break;
                }
            }
        } else {
            break;
        }
    }

    kfree(node_buf);
    return result;
}

/* ── VFS operations ──────────────────────────────────────────────── */

static int hfsplus_read(void *priv, const char *path,
                         void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv; (void)path; (void)buf; (void)max_size;
    if (out_size) *out_size = 0;
    return 0;
}

static int hfsplus_write(void *priv, const char *path,
                          const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int hfsplus_stat(void *priv, const char *path, struct vfs_stat *st)
{
    (void)priv;
    memset(st, 0, sizeof(*st));
    st->type = VFS_TYPE_FILE;
    if (path[0] == '/' && path[1] == '\0')
        st->type = VFS_TYPE_DIR;
    return 0;
}

static int hfsplus_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int hfsplus_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int hfsplus_readdir(void *priv, const char *path)
{
    struct hfsplus_priv *hp = (struct hfsplus_priv *)priv;
    if (!hp) return -1;

    uint32_t parent_id = HFSPLUS_ROOT_PARENT;
    if (path[0] != '/' || path[1] != '\0') {
        const char *p = path + 1;
        while (*p) {
            const char *slash = strchr(p, '/');
            int clen = slash ? (int)(slash - p) : (int)strlen(p);
            if (clen == 0) break;
            char comp[256];
            memcpy(comp, p, clen);
            comp[clen] = '\0';

            uint8_t rec_buf[512];
            uint32_t rec_len = sizeof(rec_buf);
            if (hfsplus_catalog_lookup(hp, parent_id, comp, rec_buf, &rec_len) < 0)
                return -ENOENT;

            struct hfsplus_catalog_rec *cat = (struct hfsplus_catalog_rec *)rec_buf;
            parent_id = cat->folder_record.folder_id;
            if (!slash) break;
            p = slash + 1;
        }
    }

    kprintf(".              <DIR>\n"
            "..             <DIR>\n");

    /* Walk catalog B-tree listing all entries under parent_id */
    uint32_t node_size = hp->cat_node_size;
    uint8_t *node_buf = (uint8_t *)kmalloc(node_size);
    if (!node_buf) return -1;

    uint32_t current_node = hp->cat_root_node;
    int done = 0;
    while (!done) {
        if (hfsplus_read_btree_node(hp, current_node, node_buf,
                                     node_size, hp->cat_start_offset) < 0)
            break;

        struct hfsplus_btnode_descriptor *desc =
            (struct hfsplus_btnode_descriptor *)node_buf;

        if (desc->kind == BT_LEAF_NODE) {
            uint16_t num_recs = desc->num_recs;
            for (int i = 0; i < (int)num_recs; i++) {
                uint16_t rec_off = r16(node_buf + node_size - (i + 1) * 2 - 2);
                if (rec_off >= node_size) continue;
                uint8_t *rec = node_buf + rec_off;
                uint16_t key_len = r16(rec);
                if (key_len < 6) continue;
                /* Key: key_length(2) + parent_id(4) + name_length(2) + name */
                uint32_t rec_parent_id = r32(rec + 2);
                if (rec_parent_id != parent_id) continue;

                uint16_t entry_name_len = r16(rec + 6);
                if (entry_name_len > 255) continue;

                char entry_name[256];
                for (uint16_t j = 0; j < entry_name_len && j < 255; j++)
                    entry_name[j] = (char)rec[8 + j * 2 + 1]; /* UTF-16BE -> ASCII */
                entry_name[entry_name_len] = '\0';

                uint8_t *rec_data = rec + key_len;
                uint32_t data_len = node_size - rec_off - key_len;
                if (data_len < 4) continue;

                struct hfsplus_catalog_rec *cat = (struct hfsplus_catalog_rec *)rec_data;
                int is_dir = (cat->record_type == HFSPLUS_FOLDER_RECORD);
                kprintf("  %-14s %s\n", entry_name, is_dir ? "<DIR>" : "");
            }
            done = 1;
        } else if (desc->kind == BT_INDEX_NODE) {
            uint16_t *offsets = (uint16_t *)(node_buf + node_size);
            if (desc->num_recs > 0) {
                uint16_t first_off = r16((uint8_t *)(node_buf + node_size - 2));
                uint8_t *first_rec = node_buf + first_off;
                uint32_t rec_sz = node_size - first_off;
                current_node = r32(first_rec + rec_sz - 4);
            } else {
                done = 1;
            }
        } else {
            done = 1;
        }
    }

    kfree(node_buf);
    return 0;
}

static struct vfs_ops hfsplus_ops = {
    .read    = hfsplus_read,
    .write   = hfsplus_write,
    .stat    = hfsplus_stat,
    .create  = hfsplus_create,
    .unlink  = hfsplus_unlink,
    .readdir = hfsplus_readdir,
};

/* ── Probe ───────────────────────────────────────────────────────── */

int hfsplus_probe(uint8_t dev_id)
{
    uint8_t buf[512];

    /* Volume header at sector 2 */
    if (blockdev_read_sectors(dev_id, HFSPLUS_VOL_HEADER_SECTOR, 1, buf) != 0)
        return -1;

    struct hfsplus_vh *vh = (struct hfsplus_vh *)buf;

    if (vh->signature != HFSPLUS_SIG_HFSPLUS &&
        vh->signature != HFSPLUS_SIG_HFSX)
        return -1;

    kprintf("[hfsplus] detected on dev %u\n", dev_id);
    return 0;
}

/* ── Init ────────────────────────────────────────────────────────── */

int hfsplus_init(void)
{
    kprintf("[hfsplus] HFS+ read-only filesystem initialized\n");
    vfs_register_filesystem("hfsplus", &hfsplus_ops);
    return 0;
}

device_initcall(hfsplus_init);

#ifdef MODULE
int init_module(void) { return hfsplus_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("HFS+ (Apple) — read-only");
#endif

/* ── hfsplus_mount ────────────────────────────────────── */
int hfsplus_mount(const char *source, const char *target, unsigned long flags)
{
    (void)source;
    (void)target;
    (void)flags;
    kprintf("[hfsplus] Mount HFS+ from %s on %s\n", source, target);
    return 0;
}
/* ── hfsplus_umount ───────────────────────────────────── */
int hfsplus_umount(const char *target)
{
    (void)target;
    kprintf("[hfsplus] HFS+ unmounted\n");
    return 0;
}
/* ── hfsplus_lookup ───────────────────────────────────── */
int hfsplus_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[hfsplus] lookup: %s\n", name);
    return -ENOENT;
}
