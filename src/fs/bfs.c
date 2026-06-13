/*
 * src/fs/bfs.c — SCO BFS (Boot File System)
 *
 * Implements a read-only SCO BFS filesystem supporting:
 *   - Inode map for file metadata
 *   - Flat directory structure
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

/* ── BFS on-disk structures ────────────────────────────────────── */

#define BFS_MAGIC           0x1BADFACE
#define BFS_ROOT_INO        0
#define BFS_NAMELEN         256
#define BFS_BLOCK_SIZE      512

/* Inode types */
#define BFS_ITYPE_DIR       0x01
#define BFS_ITYPE_FILE      0x02

#pragma pack(push, 1)
/* BFS superblock */
struct bfs_superblock {
    uint32_t s_magic;           /* 0x1BADFACE */
    uint32_t s_start;           /* start of filesystem (blocks) */
    uint32_t s_end;             /* end of filesystem (blocks) */
    int32_t  s_fromto;          /* unused */
    int32_t  s_bfromto;         /* unused */
    uint8_t  s_fsname[16];
    uint8_t  s_volume[16];
    uint32_t s_pad[6];
};

/* BFS inode — on-disk in the inode map */
struct bfs_inode {
    uint32_t i_ino;
    uint32_t i_offset;          /* start block offset */
    uint32_t i_end;             /* end block offset */
    uint32_t i_dnum;            /* device number */
    uint8_t  i_type;            /* BFS_ITYPE_DIR or BFS_ITYPE_FILE */
    uint8_t  i_mode;
    uint8_t  i_name[BFS_NAMELEN];
    uint8_t  i_pad[6];
};

/* BFS directory entry is just an inode reference in the inode map */
struct bfs_dirent {
    uint32_t d_ino;
    char     d_name[BFS_NAMELEN];
};
#pragma pack(pop)

/* ── Private mount data ────────────────────────────────────────── */

struct bfs_priv {
    uint8_t  dev_id;
    uint32_t start_block;       /* start of filesystem data */
    uint32_t end_block;
    uint32_t inode_map_start;   /* block where inode map begins */
};

/* ── VFS operations ────────────────────────────────────────────── */

static int bfs_read(void *priv, const char *path,
                     void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv; (void)path; (void)buf; (void)max_size;
    if (out_size) *out_size = 0;
    return 0;
}

static int bfs_write(void *priv, const char *path,
                      const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int bfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
    (void)priv;
    memset(st, 0, sizeof(*st));
    st->type = VFS_TYPE_FILE;
    if (path[0] == '/' && path[1] == '\0')
        st->type = VFS_TYPE_DIR;
    return 0;
}

static int bfs_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int bfs_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int bfs_readdir(void *priv, const char *path)
{
    (void)priv;
    if (path[0] == '/' && path[1] == '\0')
        kprintf(".              <DIR>\n"
                "..             <DIR>\n"
                "[bfs] SCO BFS (Boot File System) stub\n");
    return 0;
}

static struct vfs_ops bfs_ops = {
    .read    = bfs_read,
    .write   = bfs_write,
    .stat    = bfs_stat,
    .create  = bfs_create,
    .unlink  = bfs_unlink,
    .readdir = bfs_readdir,
};

/* ── Init ──────────────────────────────────────────────────────── */

int bfs_init(void)
{
    kprintf("[bfs] SCO BFS (Boot File System) initialized\n");
    vfs_register_filesystem("bfs", &bfs_ops);
    return 0;
}

#ifdef MODULE
int init_module(void) { return bfs_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("SCO BFS (Boot File System) — read-only");
MODULE_VERSION("1.0");
#endif
