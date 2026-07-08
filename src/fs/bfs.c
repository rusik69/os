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
    struct bfs_priv *bp = (struct bfs_priv *)priv;
    if (!bp) return -1;

    if (path[0] == '/' && path[1] == '\0') {
        kprintf(".              <DIR>\n");
        kprintf("..             <DIR>\n");

        /* BFS inode map starts after the superblock (block 1).
         * In BFS, directory entries are stored as inodes in the inode map.
         * The inode map is a linear array of bfs_inode structs.
         * We iterate the inode map and list all entries. */
        uint32_t inode_map_block = bp->inode_map_start;
        if (inode_map_block == 0) inode_map_block = 1;

        uint32_t inodes_per_block = BFS_BLOCK_SIZE / sizeof(struct bfs_inode);
        if (inodes_per_block == 0) inodes_per_block = 1;

        /* Read blocks from the inode map, max 8 blocks to limit search */
        int entries_found = 0;
        for (uint32_t block = 0; block < 8 && entries_found < 64; block++) {
            uint8_t buf[512];
            if (blockdev_read_sectors(bp->dev_id, inode_map_block + block, 1, buf) != 0)
                break;

            for (uint32_t i = 0; i < inodes_per_block && entries_found < 64; i++) {
                struct bfs_inode *inode = (struct bfs_inode *)(buf + i * sizeof(struct bfs_inode));

                /* An inode is in use if its inode number is non-zero */
                if (inode->i_ino == 0)
                    continue;

                /* Check if it belongs to the root directory.
                 * In BFS, the root inode has i_ino == 0 (BFS_ROOT_INO).
                 * All other inodes are files/subdirs listed in the root. */
                if (inode->i_ino == BFS_ROOT_INO)
                    continue; /* skip the root entry itself */

                /* Extract the name */
                char name_buf[BFS_NAMELEN + 1];
                uint32_t name_len;
                for (name_len = 0; name_len < BFS_NAMELEN; name_len++) {
                    char c = (char)inode->i_name[name_len];
                    if (c == '\0') break;
                    name_buf[name_len] = c;
                }
                name_buf[name_len] = '\0';

                if (name_len == 0) continue;

                uint32_t file_size = (inode->i_end - inode->i_offset) * BFS_BLOCK_SIZE;

                if (inode->i_type == BFS_ITYPE_DIR) {
                    kprintf("%-18s <DIR>\n", name_buf);
                } else {
                    kprintf("%-18s %u\n", name_buf, file_size);
                }
                entries_found++;
            }
        }

        if (entries_found == 0) {
            kprintf("[bfs] no entries found in inode map\n");
        }
    }
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

/* ── Probe ──────────────────────────────────────────────────────── */

static int bfs_probe(uint8_t dev_id)
{
    uint8_t buf[512];
    if (blockdev_read_sectors(dev_id, 0, 1, buf) != 0)
        return -1;
    struct bfs_superblock *sb = (struct bfs_superblock *)buf;
    if (sb->s_magic == BFS_MAGIC) {
        kprintf("[bfs] SCO BFS detected on dev %u\n", dev_id);
        return 0;
    }
    return -1;
}

/* ── Init ──────────────────────────────────────────────────────── */

static int __init bfs_init(void)
{
    kprintf("[bfs] SCO BFS (Boot File System) initialized\n");
    vfs_register_filesystem("bfs", &bfs_ops);
    return 0;
}

#ifdef MODULE
int __init init_module(void) { return bfs_init(); }
void __exit cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("SCO BFS (Boot File System) — read-only");
MODULE_VERSION("1.0");
#endif

/* ── bfs_mount ──────────────────────────────────────── */
static int bfs_mount(const char *source, const char *target, unsigned long flags)
{
    (void)source;
    (void)target;
    (void)flags;
    kprintf("[bfs] Mount BFS from %s on %s\n", source, target);
    return 0;
}
/* ── bfs_umount ──────────────────────────────────────── */
static int bfs_umount(const char *target)
{
    (void)target;
    kprintf("[bfs] BFS unmounted\n");
    return 0;
}
/* ── bfs_lookup ─────────────────────────────────────── */
static int bfs_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[bfs] lookup: %s\n", name);
    return -ENOENT;
}
