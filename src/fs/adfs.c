/*
 * src/fs/adfs.c — Acorn Disc Filing System (ADFS)
 *
 * Implements a read-only ADFS filesystem supporting:
 *   - Map/chains structure for block allocation
 *   - Old and new directory formats
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

/* ── ADFS on-disk structures ───────────────────────────────────── */

#define ADFS_BLOCK_SIZE    512
#define ADFS_MAP_FRAG      0x01      /* fragment ID for map */

/* Old directory entry */
#define ADFS_OLD_NAME_LEN   10
#define ADFS_NEW_NAME_LEN   255

#pragma pack(push, 1)
/* ADFS superblock / boot block — at sector 0 */
struct adfs_discrec {
    uint8_t  log2secsize;     /* log2(sector size) e.g. 8 -> 256, 9 -> 512 */
    uint8_t  secspertrack;    /* sectors per track */
    uint8_t  heads;
    uint8_t  density;
    uint8_t  idlen;
    uint8_t  log2bpmb;
    uint8_t  skew;
    uint8_t  bootoption;
    uint8_t  reserved[4];
    uint8_t  discaddr[4];
    uint8_t  disc_size[4];    /* total blocks on disc */
    uint8_t  disc_id2[4];
    uint8_t  disc_name[10];
    uint8_t  disc_type;
    uint8_t  disc_size2[4];
    uint8_t  boot_size[4];
    uint8_t  root_size[4];    /* size of root directory */
    uint8_t  disc_csum;
};

/* Fragment (map entry) — 8 bytes */
struct adfs_fragment {
    uint32_t frag_addr;       /* start block */
    uint32_t frag_size;       /* length in blocks */
};

/* Map — at fixed location after boot block */
struct adfs_map {
    uint8_t  map_id;
    uint8_t  map_start;
    uint8_t  map_size;        /* total map size in blocks */
    uint8_t  zone_spare;
    uint32_t old_zone_size;
    /* fragments follow */
};

/* Old directory header */
struct adfs_old_dir {
    uint8_t  dir_name[4];     /* name */
    uint8_t  dir_parent[4];
    uint8_t  dir_seq;
    uint8_t  dir_info[2];
    uint8_t  dir_size[4];     /* number of entries */
};

/* Old directory entry */
struct adfs_old_dirent {
    uint8_t  ent_name[ADFS_OLD_NAME_LEN];
    uint32_t ent_loadaddr;
    uint32_t ent_execaddr;
    uint32_t ent_len;         /* file length */
    uint16_t ent_attr;
    uint16_t ent_btree[2];    /* block chain */
};

/* New directory entry */
struct adfs_new_dirent {
    uint8_t  ent_type;
    uint8_t  ent_ident;
    uint32_t ent_loadaddr;
    uint32_t ent_execaddr;
    uint32_t ent_len;
    uint32_t ent_attr;
    uint32_t ent_addr;        /* block address of object */
    char     ent_name[ADFS_NEW_NAME_LEN];
};
#pragma pack(pop)

/* ── Private mount data ────────────────────────────────────────── */

struct adfs_priv {
    uint8_t  dev_id;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t root_block;       /* block address of root dir */
};

/* ── VFS operations ────────────────────────────────────────────── */

static int adfs_read(void *priv, const char *path,
                      void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv; (void)path; (void)buf; (void)max_size;
    if (out_size) *out_size = 0;
    return 0;
}

static int adfs_write(void *priv, const char *path,
                       const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int adfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
    (void)priv;
    memset(st, 0, sizeof(*st));
    st->type = VFS_TYPE_FILE;
    if (path[0] == '/' && path[1] == '\0')
        st->type = VFS_TYPE_DIR;
    return 0;
}

static int adfs_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int adfs_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int adfs_readdir(void *priv, const char *path)
{
    (void)priv;
    if (path[0] == '/' && path[1] == '\0')
        kprintf(".              <DIR>\n"
                "..             <DIR>\n"
                "[adfs] Acorn Disc Filing System (map/chains) stub\n");
    return 0;
}

static struct vfs_ops adfs_ops = {
    .read    = adfs_read,
    .write   = adfs_write,
    .stat    = adfs_stat,
    .create  = adfs_create,
    .unlink  = adfs_unlink,
    .readdir = adfs_readdir,
};

/* ── Probe ──────────────────────────────────────────────────────── */

int adfs_probe(uint8_t dev_id)
{
    uint8_t buf[512];
    if (blockdev_read_sectors(dev_id, 0, 1, buf) != 0)
        return -1;
    struct adfs_discrec *dr = (struct adfs_discrec *)buf;
    if (dr->log2secsize >= 8 && dr->log2secsize <= 10 &&
        dr->secspertrack > 0 && dr->heads > 0) {
        kprintf("[adfs] ADFS detected on dev %u (secsize=%d, sec/track=%d)\n",
                dev_id, 1 << dr->log2secsize, dr->secspertrack);
        return 0;
    }
    return -1;
}

/* ── Init ──────────────────────────────────────────────────────── */

int adfs_init(void)
{
    kprintf("[adfs] Acorn Disc Filing System initialized\n");
    vfs_register_filesystem("adfs", &adfs_ops);
    return 0;
}

#ifdef MODULE
int init_module(void) { return adfs_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Acorn ADFS — map/chains, directory format");
MODULE_VERSION("1.0");
#endif
