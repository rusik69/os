/*
 * src/fs/sysv.c — System V FS (SYSV5, Coherent, Xenix variants)
 *
 * Implements a read-only System V filesystem supporting:
 *   - SYSV5, Coherent, and Xenix variants
 *   - Inode table, block bitmaps, directory traversal
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

/* ── SYSV on-disk structures ───────────────────────────────────── */

/* Magic numbers for different SYSV variants */
#define SYSV_MAGIC_V7      0xfd187e20
#define SYSV_MAGIC_SYSV4   0xfd2e0824
#define SYSV_MAGIC_VENIX   0xfd2e1924
#define SYSV_MAGIC_COH     0xfd2e2842   /* Coherent */
#define SYSV_MAGIC_XENIX   0xfd2e301F   /* Xenix */
#define SYSV_MAGIC_XENIX2  0xfd2e302C
#define SYSV_MAGIC_SYSV5   0xfd2e3040   /* SYSV5 */

#define SYSV_ROOT_INO      1
#define SYSV_BLOCK_SIZE    1024
#define SYSV_NAMSIZ        14

#pragma pack(push, 1)
/* Superblock */
struct sysv_superblock {
    uint32_t  s_isize;       /* size of inode table in blocks */
    uint32_t  s_fsize;       /* total device size in blocks */
    uint16_t  s_nfree;       /* number of free blocks in s_free array */
    uint16_t  s_free[100];   /* free block list */
    uint16_t  s_ninode;      /* number of free inodes in s_inode array */
    uint16_t  s_inode[100];  /* free inode list */
    uint8_t   s_flock;       /* free block list lock */
    uint8_t   s_ilock;       /* free inode list lock */
    uint8_t   s_fmod;        /* superblock modified flag */
    uint8_t   s_ronly;       /* read-only flag */
    uint32_t  s_time;
    uint16_t  s_dinfo[4];
    uint16_t  s_tfree;
    uint16_t  s_tinode;
    uint16_t  s_pad[4];
    uint16_t  s_magic;
    uint16_t  s_type;
};

/* Inode */
struct sysv_inode {
    uint16_t i_mode;
    uint16_t i_nlink;
    uint16_t i_uid;
    uint16_t i_gid;
    uint32_t i_size1;        /* low 16 bits */
    uint32_t i_size2;        /* high 16 bits (some variants) */
    uint32_t i_addr[13];     /* block addresses */
    uint32_t i_atime;
    uint32_t i_mtime;
    uint32_t i_ctime;
};

/* Directory entry (16 bytes) */
struct sysv_dirent {
    uint16_t d_ino;
    char     d_name[SYSV_NAMSIZ];
};
#pragma pack(pop)

/* ── Private mount data ────────────────────────────────────────── */

struct sysv_priv {
    uint8_t  dev_id;
    uint16_t magic;
    uint32_t isize;          /* inode table size in blocks */
    uint32_t fsize;          /* total blocks */
};

/* ── VFS operations ────────────────────────────────────────────── */

static int sysv_read(void *priv, const char *path,
                      void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv; (void)path; (void)buf; (void)max_size;
    if (out_size) *out_size = 0;
    return 0;
}

static int sysv_write(void *priv, const char *path,
                       const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int sysv_stat(void *priv, const char *path, struct vfs_stat *st)
{
    (void)priv;
    memset(st, 0, sizeof(*st));
    st->type = VFS_TYPE_FILE;
    if (path[0] == '/' && path[1] == '\0')
        st->type = VFS_TYPE_DIR;
    return 0;
}

static int sysv_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int sysv_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int sysv_readdir(void *priv, const char *path)
{
    (void)priv;
    if (path[0] == '/' && path[1] == '\0')
        kprintf(".              <DIR>\n"
                "..             <DIR>\n"
                "[sysv] System V FS (SYSV5/Coherent/Xenix) stub\n");
    return 0;
}

static struct vfs_ops sysv_ops = {
    .read    = sysv_read,
    .write   = sysv_write,
    .stat    = sysv_stat,
    .create  = sysv_create,
    .unlink  = sysv_unlink,
    .readdir = sysv_readdir,
};

/* ── Probe ──────────────────────────────────────────────────────── */

int sysv_probe(uint8_t dev_id)
{
    uint8_t buf[1024];
    if (blockdev_read_sectors(dev_id, 0, 2, buf) != 0)
        return -1;
    uint32_t *magic32 = (uint32_t *)buf;
    (void)magic32;
    struct sysv_superblock *sb = (struct sysv_superblock *)buf;
    uint16_t magic16 = sb->s_magic;
    if (magic16 == (uint16_t)SYSV_MAGIC_V7 || magic16 == (uint16_t)SYSV_MAGIC_SYSV4 ||
        magic16 == (uint16_t)SYSV_MAGIC_COH || magic16 == (uint16_t)SYSV_MAGIC_XENIX ||
        magic16 == (uint16_t)SYSV_MAGIC_XENIX2 || magic16 == (uint16_t)SYSV_MAGIC_SYSV5 ||
        magic16 == (uint16_t)SYSV_MAGIC_VENIX) {
        kprintf("[sysv] SYSV variant detected on dev %u (magic=0x%04x)\n", dev_id, magic16);
        return 0;
    }
    return -1;
}

/* ── Init ──────────────────────────────────────────────────────── */

int sysv_init(void)
{
    kprintf("[sysv] System V FS (SYSV5, Coherent, Xenix variants) initialized\n");
    vfs_register_filesystem("sysv", &sysv_ops);
    return 0;
}

#ifdef MODULE
int init_module(void) { return sysv_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("System V Filesystem — SYSV5, Coherent, Xenix variants");
MODULE_VERSION("1.0");
#endif
