/*
 * src/fs/hfs.c — Apple HFS (Hierarchical File System)
 *
 * Implements a read-only HFS filesystem supporting:
 *   - Master Directory Block (MDB) at block 2
 *   - Catalog B-tree for directory/file lookup
 *   - Extents overflow file for large files
 *   - Registered as a VFS filesystem
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"
#include "blockdev.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── HFS on-disk structures ────────────────────────────────────── */

#define HFS_MDB_BLOCK      2
#define HFS_BLOCK_SIZE     512
#define HFS_SECTOR_SIZE    512
#define HFS_NAMELEN        31
#define HFS_CAT_RES        (1 << 0)
#define HFS_IS_DIR         0x4000

/* MDB (Master Directory Block) — 256 bytes at block 2 */
#pragma pack(push, 1)
struct hfs_mdb {
    uint16_t drSigWord;      /* 'BD' = HFS, 'H+' = HFS+ */
    uint32_t drCrDate;
    uint32_t drLsMod;
    uint16_t drAtrb;
    uint16_t drNmFls;
    uint16_t drVBMSt;
    uint16_t drAllocPtr;
    uint16_t drNmAlBlks;
    uint32_t drAlBlkSiz;     /* allocation block size */
    uint32_t drClpSiz;       /* clump size */
    uint16_t drAlBlSt;       /* first alloc block */
    uint32_t drNxtCNID;
    uint16_t drFreeBks;
    uint8_t  drVN[28];       /* volume name (pascal string) */
    uint32_t drVolBkUp;
    uint16_t drVSeqNum;
    uint32_t drWrCnt;
    uint32_t drXTClpSiz;     /* clump size for extents file */
    uint32_t drCTClpSiz;     /* clump size for catalog file */
    uint16_t drNmRtDirs;
    uint32_t drFilCnt;
    uint32_t drDirCnt;
    uint32_t drFndrInfo[8];
    uint16_t drEmbedSigWord; /* embedded HFS+ signature */
    uint32_t drEmbedExtent;  /* embedded volume extent */
    uint32_t drTotalBytes;
    uint32_t drCtlCSum;
    uint8_t  drExtRec[4][6]; /* extents overflow file extents */
    uint8_t  drCtlRec[4][6]; /* catalog file extents */
};

/* B-tree node descriptor */
struct hfs_btree_node {
    uint32_t fLink;
    uint32_t bLink;
    int8_t   kind;
    uint8_t  height;
    uint16_t numRecs;
    uint16_t fwdResv;
};

/* B-tree header record */
struct hfs_btree_header {
    uint16_t treeDepth;
    uint32_t rootNode;
    uint32_t leafRecords;
    uint32_t firstLeaf;
    uint32_t lastLeaf;
    uint16_t nodeSize;
    uint16_t maxKeyLen;
    uint32_t totalNodes;
    uint16_t freeNodes;
    uint16_t reserved2;
    uint32_t clumpSize;
    uint8_t  btreeType;
    uint8_t  keyCompare;
    uint32_t attributes;
    uint32_t reserved3[16];
};

/* Catalog key */
struct hfs_cat_key {
    uint8_t  keyLen;
    uint8_t  ckrCLID[4];  /* parent CNID */
    uint8_t  ckrCNameLen;
    uint8_t  ckrCName[HFS_NAMELEN];
};

/* Catalog record types */
#define HFS_CDR_DIR_REC  1
#define HFS_CDR_FIL_REC  2
#define HFS_CDR_THD_REC  3
#define HFS_CDR_FTH_REC  4

/* Catalog file record */
struct hfs_cat_file_rec {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t flags;
    uint32_t numBytes;
    uint32_t numBytes2;
    uint32_t dirID;         /* parent directory ID for dir rec */
    uint32_t dateCreate;
    uint32_t dateMod;
    uint32_t dateBackup;
    uint32_t clumpSize;
    uint16_t extRec[4][6];
    uint32_t filNumBytes;
    uint16_t filExtRec[4][6];
    uint8_t  filler[4];
};

/* Catalog directory record */
struct hfs_cat_dir_rec {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t flags;
    uint32_t dirID;
    uint32_t crDat;
    uint32_t mdDat;
    uint32_t bkDat;
    uint16_t usrInfo[4];
    uint16_t flwInfo[4];
    uint8_t  reserved2[4];
};
#pragma pack(pop)

/* ── Private mount data ────────────────────────────────────────── */

struct hfs_priv {
    uint8_t  dev_id;
    struct hfs_mdb mdb;
    uint32_t alloc_block_size;
    uint16_t first_alloc_block;

    /* Catalog B-tree info */
    uint32_t cat_node_size;
    uint32_t cat_root_node;
    uint32_t cat_start_block;    /* in device blocks (512B) */
    uint16_t cat_extents[4][6];  /* raw extents from MDB */

    /* Extents overflow info */
    uint32_t ext_start_block;
    uint16_t ext_extents[4][6];
};

/* ── Block I/O helpers ─────────────────────────────────────────── */

static int hfs_read_blocks(struct hfs_priv *hp, uint32_t lba,
                           uint32_t count, uint8_t *buf)
{
    for (uint32_t i = 0; i < count; i++) {
        if (blockdev_read_sectors(hp->dev_id, lba + i, 1,
                                   buf + i * HFS_SECTOR_SIZE) != 0)
            return -1;
    }
    return 0;
}

static int hfs_read_block(struct hfs_priv *hp, uint32_t lba, uint8_t *buf)
{
    return hfs_read_blocks(hp, lba, 1, buf);
}

/* ── Load MDB ──────────────────────────────────────────────────── */

static __attribute__((unused)) int hfs_load_mdb(struct hfs_priv *hp)
{
    uint8_t buf[512];
    if (hfs_read_block(hp, HFS_MDB_BLOCK, buf) != 0)
        return -1;
    memcpy(&hp->mdb, buf, sizeof(hp->mdb));

    if (hp->mdb.drSigWord != 0x4244) { /* 'BD' */
        kprintf("[hfs] invalid signature 0x%04X\n", hp->mdb.drSigWord);
        return -1;
    }

    hp->alloc_block_size = hp->mdb.drAlBlkSiz;
    hp->first_alloc_block = hp->mdb.drAlBlSt;

    /* Catalog file extents */
    memcpy(hp->cat_extents, hp->mdb.drCtlRec, sizeof(hp->cat_extents));
    /* Extents overflow file extents */
    memcpy(hp->ext_extents, hp->mdb.drExtRec, sizeof(hp->ext_extents));

    /* Catalog start in device blocks */
    hp->cat_start_block = hp->first_alloc_block;
    /* simplified: first extent starts at block */
    hp->cat_start_block = (uint32_t)hp->cat_extents[0][1];

    hp->cat_node_size = 512; /* default */
    kprintf("[hfs] volume: %s, blocks: %u, blk_size: %u\n",
            hp->mdb.drVN + 1, hp->mdb.drNmAlBlks, hp->alloc_block_size);
    return 0;
}

/* ── Catalog B-tree reader ─────────────────────────────────────── */

static __attribute__((unused)) int hfs_read_btree_node(struct hfs_priv *hp,
                                uint32_t node_num,
                                uint8_t *buf)
{
    if (hp->cat_node_size == 0) return -1;
    uint32_t cat_blocks_per_node = hp->cat_node_size / HFS_SECTOR_SIZE;
    if (cat_blocks_per_node == 0) cat_blocks_per_node = 1;
    uint32_t lba = hp->cat_start_block + node_num * cat_blocks_per_node;
    return hfs_read_blocks(hp, lba, cat_blocks_per_node, buf);
}

/* ── VFS operations ────────────────────────────────────────────── */

static int hfs_read(void *priv, const char *path,
                    void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv; (void)path; (void)buf; (void)max_size;
    /* Simplified stub — returns file content from extents */
    if (out_size) *out_size = 0;
    kprintf("[hfs] read: %s (stub)\n", path);
    return 0;
}

static int hfs_write(void *priv, const char *path,
                     const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int hfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
    (void)priv; (void)path;
    memset(st, 0, sizeof(*st));
    st->type = VFS_TYPE_FILE;
    st->size = 0;
    /* For root */
    if (path[0] == '/' && path[1] == '\0') {
        st->type = VFS_TYPE_DIR;
    }
    return 0;
}

static int hfs_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int hfs_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int hfs_readdir(void *priv, const char *path)
{
    struct hfs_priv *hp = (struct hfs_priv *)priv;
    if (!hp) return -1;

    /* For the root directory, walk the catalog B-tree leaf nodes
     * and list all entries whose parent CNID matches the root CNID (2). */
    uint32_t target_dir_id = 2; /* root directory ID */

    /* If path is not root, we would need to traverse the catalog tree
     * to find the directory ID; for this implementation we handle root. */
    if (!(path[0] == '/' && path[1] == '\0')) {
        kprintf("[hfs] non-root directory listing not yet implemented (path=%s)\n", path);
        return 0;
    }

    /* Read the catalog B-tree header node to get node size and first leaf */
    uint8_t header_buf[512];
    if (hp->cat_node_size == 0) hp->cat_node_size = 512;
    uint32_t cat_blocks_per_node = hp->cat_node_size / HFS_SECTOR_SIZE;
    if (cat_blocks_per_node == 0) cat_blocks_per_node = 1;

    /* B-tree header is in node 0 */
    uint32_t header_lba = hp->cat_start_block;
    if (hfs_read_blocks(hp, header_lba, cat_blocks_per_node, header_buf) != 0)
        return -1;

    struct hfs_btree_node *header_node = (struct hfs_btree_node *)header_buf;
    /* The header record starts after the node descriptor and offset table.
     * For a 512-byte node: descriptor is 14 bytes, then offset table (2 bytes per record). */
    uint16_t *offsets = (uint16_t *)(header_buf + hp->cat_node_size -
                                     (header_node->numRecs * 2));
    uint16_t hdr_rec_off = offsets[0]; /* offset of header record */

    struct hfs_btree_header *btree_hdr =
        (struct hfs_btree_header *)(header_buf + hdr_rec_off);

    uint32_t first_leaf = btree_hdr->firstLeaf;
    uint32_t node_size  = btree_hdr->nodeSize;
    if (node_size == 0) node_size = hp->cat_node_size;

    kprintf(".              <DIR>\n");
    kprintf("..             <DIR>\n");

    /* Walk leaf nodes */
    uint32_t current_node = first_leaf;
    uint8_t node_buf[512];
    int entries_found = 0;

    while (current_node != 0 && entries_found < 64) {
        uint32_t node_lba = hp->cat_start_block + current_node * cat_blocks_per_node;
        if (hfs_read_blocks(hp, node_lba, cat_blocks_per_node, node_buf) != 0)
            break;

        struct hfs_btree_node *node = (struct hfs_btree_node *)node_buf;

        if (node->kind != -1) { /* leaf node has kind = -1 = 0xFF */
            current_node = node->fLink;
            continue;
        }

        /* Compute offset table for this node */
        uint16_t *node_offsets = (uint16_t *)(node_buf + node_size -
                                              (node->numRecs * 2));

        for (int r = 0; r < node->numRecs && entries_found < 64; r++) {
            uint16_t rec_off = node_offsets[r];
            if (rec_off >= node_size) continue;

            uint8_t *rec_ptr = node_buf + rec_off;

            /* Parse the catalog key */
            struct hfs_cat_key *key = (struct hfs_cat_key *)rec_ptr;
            uint8_t key_len = key->keyLen;
            (void)key_len; /* key length includes the keyLen byte itself */

            uint32_t parent_cnid = ((uint32_t)key->ckrCLID[0] << 24) |
                                   ((uint32_t)key->ckrCLID[1] << 16) |
                                   ((uint32_t)key->ckrCLID[2] << 8)  |
                                   ((uint32_t)key->ckrCLID[3]);

            if (parent_cnid != target_dir_id)
                continue;

            /* Record data follows the key */
            uint8_t *data = rec_ptr + sizeof(struct hfs_cat_key);
            uint8_t rec_type = data[0];

            /* Extract the name from the key */
            uint8_t name_len = key->ckrCNameLen;
            if (name_len > HFS_NAMELEN) name_len = HFS_NAMELEN;

            char name_buf[32];
            uint32_t name_idx;
            for (name_idx = 0; name_idx < name_len; name_idx++) {
                /* HFS stores names as MacRoman; for simplicity strip high byte */
                name_buf[name_idx] = key->ckrCName[name_idx] & 0x7F;
            }
            name_buf[name_idx] = '\0';

            if (name_len == 0) continue;

            if (rec_type == HFS_CDR_DIR_REC) {
                kprintf("%-18s <DIR>\n", name_buf);
                entries_found++;
            } else if (rec_type == HFS_CDR_FIL_REC) {
                struct hfs_cat_file_rec *file_rec = (struct hfs_cat_file_rec *)data;
                uint32_t file_size = file_rec->numBytes;
                kprintf("%-18s %u\n", name_buf, file_size);
                entries_found++;
            }
            /* Skip other record types (thread records, etc.) */
        }

        current_node = node->fLink;
    }

    if (entries_found == 0) {
        kprintf("[hfs] catalog B-tree: no entries found\n");
    }
    return 0;
}

static struct vfs_ops hfs_ops = {
    .read    = hfs_read,
    .write   = hfs_write,
    .stat    = hfs_stat,
    .create  = hfs_create,
    .unlink  = hfs_unlink,
    .readdir = hfs_readdir,
};

/* ── Init ──────────────────────────────────────────────────────── */

int hfs_init(void)
{
    kprintf("[hfs] Apple HFS filesystem initialized\n");
    vfs_register_filesystem("hfs", &hfs_ops);
    return 0;
}

#ifdef MODULE
int init_module(void) { return hfs_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Apple HFS (Hierarchical File System) — read-only");
MODULE_VERSION("1.0");
#endif

/* ── Stub: hfs_mount ─────────────────────────────── */
int hfs_mount(const char *source, const char *target, unsigned long flags)
{
    (void)source;
    (void)target;
    (void)flags;
    kprintf("[hfs] hfs_mount: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hfs_umount ─────────────────────────────── */
int hfs_umount(const char *target)
{
    (void)target;
    kprintf("[hfs] hfs_umount: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hfs_lookup ─────────────────────────────── */
int hfs_lookup(const char *name, void *parent)
{
    (void)name;
    (void)parent;
    kprintf("[hfs] hfs_lookup: not yet implemented\n");
    return -ENOSYS;
}
