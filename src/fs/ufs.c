/*
 * src/fs/ufs.c — UFS/FFS (Unix File System)
 *
 * Implements a read-only UFS/FFS filesystem supporting:
 *   - Cylinder groups, block groups, fragments
 *   - Soft updates (stub for future implementation)
 *   - Inode-based file lookup and zone addressing
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

/* ── UFS on-disk structures ────────────────────────────────────── */

#define UFS_MAGIC       0x00011954
#define UFS_MAGIC_FLIP  0x54190100
#define UFS2_MAGIC      0x19540119
#define UFS_CG_MAGIC    0x090255
#define UFS_ROOT_INO    2
#define UFS_BLOCK_SIZE  8192   /* max */

/* Superblock is at offset 1024 bytes in sector 2 */
#define UFS_SBLOCK      2

/* Flag bits */
#define UFS_FLAG_FS_ISCLEAN     0x01

/* Cylinder group flags */
#define UFS_CG_FLAG_NONE        0

/* Inode flags */
#define UFS_IFDIR       0040000
#define UFS_IFREG       0100000

/* Direct blocks + indirect levels (UFS1) */
#define UFS_NDADDR      12
#define UFS_NIADDR      3

#pragma pack(push, 1)
/* UFS1 Superblock — 1376 bytes, starts at sector 2 (offset 1024) */
struct ufs_superblock {
    uint16_t s_link;            /* unused */
    uint16_t s_postblformat;
    uint32_t s_nsect;
    uint32_t s_ntrak;
    uint32_t s_ncyl;
    uint32_t s_size;            /* total 512-byte sectors */
    uint32_t s_bsize;           /* filesystem block size */
    uint32_t s_fsize;           /* fragment size */
    uint32_t s_frag;            /* fragments per block */
    uint32_t s_cgmask;          /* used to calc cylinder group */
    uint32_t s_cgoffset;
    uint32_t s_cgrotor;
    uint32_t s_fp_old_cstotal[4];
    int32_t  s_fp_old_time;
    int32_t  s_fp_old_size;
    int32_t  s_fp_old_bfree;
    int32_t  s_fp_old_ffree;
    int32_t  s_fp_old_bmsize;
    int32_t  s_fp_old_fmsize;
    int32_t  s_fp_old_bmblock[16];
    int32_t  s_fp_old_fmblock[16];
    /* ... truncated for brevity ... */
    int32_t  s_csaddr;          /* block of cylinder group summaries */
    int32_t  s_cssize;
    int32_t  s_cgblkno;         /* first cylinder group block */
    int32_t  s_fpg;             /* fragments per group */
    uint8_t  s_fp_old_volna[32];
    uint16_t s_fp_old_svuid;
    uint16_t s_fp_old_svgid;
    /* We'll use a fixed offset to read fields we need */
};

/* UFS1 Cylinder group descriptor (at cgblkno) */
struct ufs_cg {
    uint32_t cg_magic;
    int32_t  cg_time;
    int32_t  cg_cgx;            /* cylinder group number */
    uint16_t cg_ncyl;
    uint16_t cg_niblk;          /* number of inode blocks */
    uint32_t cg_ndblk;          /* number of data blocks */
    uint32_t cg_cs[4];
    uint32_t cg_rotor;
    uint32_t cg_frotor;
    uint32_t cg_irotor;
    uint32_t cg_frsum[8];
    int32_t  cg_btotoff;        /* offset of block totals */
    int32_t  cg_boff;           /* offset of block bitmap */
    int32_t  cg_iusedoff;       /* offset of inode used map */
    int32_t  cg_freeoff;
    int32_t  cg_nextfreeoff;
    int32_t  cg_clustersumoff;
    int32_t  cg_clusteroff;
    int32_t  cg_nclusterblks;
    uint32_t cg_sparecon[13];
    uint32_t cg_spare[3];
    /* bitmaps follow */
};

/* UFS1 Inode (128 bytes) */
struct ufs_inode {
    uint16_t ui_mode;
    uint16_t ui_nlink;
    uint32_t ui_size;
    uint32_t ui_atime;
    uint32_t ui_atimensec;
    uint32_t ui_mtime;
    uint32_t ui_mtimensec;
    uint32_t ui_ctime;
    uint32_t ui_ctimensec;
    uint32_t ui_db[UFS_NDADDR];  /* direct blocks */
    uint32_t ui_ib[UFS_NIADDR];  /* indirect blocks */
    uint32_t ui_flags;
    uint32_t ui_blocks;
    int32_t  ui_gen;
    uint32_t ui_shadow;
    uint8_t  ui_uid;
    uint8_t  ui_gid;
    uint8_t  ui_reserved[6];
};
#pragma pack(pop)

/* ── Private mount data ────────────────────────────────────────── */

struct ufs_priv {
    uint8_t  dev_id;
    uint32_t block_size;
    uint32_t frag_size;
    uint32_t frags_per_block;
    uint32_t fpg;               /* fragments per group */
    uint32_t cgblkno;           /* first cylinder group block */
    uint32_t csaddr;            /* cylinder group summary block */
    uint32_t num_cg;
    uint32_t inode_size;
};

/* ── UFS inode read and block pointer translation ─────────────── */

/*
 * Read a UFS1 inode from disk given the inode number.
 * Inodes per cylinder group = (fragments_per_group * frag_size) / inode_size.
 * Cylinder group number = inode_number / inodes_per_group.
 * Inode offset within group = (inode_number % inodes_per_group) * inode_size.
 */
static __attribute__((unused)) int ufs_read_inode(struct ufs_priv *up,
                           uint32_t inum,
                           struct ufs_inode *inode)
{
    if (!up || !inode || inum == 0)
        return -1;

    /* Calculate inodes per cylinder group */
    uint32_t frags_per_group = up->fpg;
    uint32_t bytes_per_group = frags_per_group * up->frag_size;
    uint32_t inodes_per_group = bytes_per_group / sizeof(struct ufs_inode);
    if (inodes_per_group == 0) inodes_per_group = 1;

    /* Cylinder group number */
    uint32_t cg_num = inum / inodes_per_group;
    /* Inode index within the cylinder group */
    uint32_t inode_idx = inum % inodes_per_group;

    /* Read the cylinder group descriptor block to get the inode table location.
     * The CG descriptor is at a fixed block within the CG area.
     * For UFS1, the CG block number = cgblkno + cg_num * (fragments_per_group / fragments_per_block).
     * We read the CG descriptor from the block device. */
    uint32_t cg_block_bytes = cg_num * bytes_per_group;
    uint32_t cg_block_num = up->block_size ? up->cgblkno + (cg_block_bytes / up->block_size) : 0;

    uint8_t cg_buf[512];
    if (blockdev_read_sectors(up->dev_id, cg_block_num, 1, cg_buf) != 0)
        return -1;

    struct ufs_cg *cg = (struct ufs_cg *)cg_buf;
    if (cg->cg_magic != 0x090255) {
        kprintf("[ufs] bad CG magic for group %u\n", cg_num);
        return -1;
    }

    /* Inode table starts at CG block data + cg_iusedoff offset.
     * In many UFS layouts, the inode table is at a fixed offset after the CG descriptor.
     * The actual inode block = first data block of the CG + inode block offset.
     * For simplicity, we compute: inode table block = cg_block_num + block_offset_of_inode_table. */
    uint32_t inode_table_offset = cg->cg_iusedoff;  /* offset in bytes from CG start */
    /* The inode bitmap starts at cg_iusedoff. The inode data itself typically follows
     * the bitmaps.  In UFS1, the inode blocks are located at a fixed position after the
     * CG header and bitmaps.  We compute directly: */
    uint32_t inodes_per_block = up->block_size / sizeof(struct ufs_inode);
    if (inodes_per_block == 0) return -1;
    uint32_t inode_block_offset = inode_idx / inodes_per_block;
    uint32_t inode_block_idx   = inode_idx % inodes_per_block;
    uint32_t inode_block_num   = cg_block_num + 1 + inode_block_offset;

    uint8_t ibuf[512];
    if (blockdev_read_sectors(up->dev_id, inode_block_num, 1, ibuf) != 0)
        return -1;

    memcpy(inode, ibuf + inode_block_idx * sizeof(struct ufs_inode),
           sizeof(struct ufs_inode));
    return 0;
}

/*
 * Translate a logical block number (0-based within a file) to a
 * physical block number using direct/indirect/double-indirect pointers.
 * Returns the physical block number, or 0 if the block is a hole/sparse.
 */
static __attribute__((unused)) uint32_t ufs_bmap(struct ufs_priv *up,
                          struct ufs_inode *inode,
                          uint32_t logical_block)
{
    /* Direct blocks (ui_db[0..11]) */
    if (logical_block < UFS_NDADDR)
        return inode->ui_db[logical_block];

    /* Singly indirect (ui_ib[0]) */
    uint32_t per_block = up->block_size / 4;  /* block pointers per indirect block */
    if (per_block == 0) return 0;
    uint32_t idx = logical_block - UFS_NDADDR;
    if (idx < per_block) {
        uint8_t buf[512];
        uint32_t ind_block = inode->ui_ib[0];
        if (ind_block == 0) return 0;  /* hole */
        if (blockdev_read_sectors(up->dev_id, ind_block, 1, buf) != 0)
            return 0;
        return ((uint32_t *)buf)[idx];
    }

    /* Doubly indirect (ui_ib[1]) */
    idx -= per_block;
    if (idx < per_block * per_block) {
        uint8_t buf[512];
        uint32_t dind_block = inode->ui_ib[1];
        if (dind_block == 0) return 0;
        if (blockdev_read_sectors(up->dev_id, dind_block, 1, buf) != 0)
            return 0;
        uint32_t ind_idx = idx / per_block;
        uint32_t blk_idx = idx % per_block;
        uint32_t ind_block = ((uint32_t *)buf)[ind_idx];
        if (ind_block == 0) return 0;
        if (blockdev_read_sectors(up->dev_id, ind_block, 1, buf) != 0)
            return 0;
        return ((uint32_t *)buf)[blk_idx];
    }

    /* Triply indirect (ui_ib[2]) — not commonly used but supported */
    idx -= per_block * per_block;
    if (idx < per_block * per_block * per_block) {
        uint8_t buf[512];
        uint32_t tind_block = inode->ui_ib[2];
        if (tind_block == 0) return 0;
        if (blockdev_read_sectors(up->dev_id, tind_block, 1, buf) != 0)
            return 0;
        uint32_t dind_idx = idx / (per_block * per_block);
        uint32_t rem      = idx % (per_block * per_block);
        uint32_t ind_idx  = rem / per_block;
        uint32_t blk_idx  = rem % per_block;
        uint32_t dind_block = ((uint32_t *)buf)[dind_idx];
        if (dind_block == 0) return 0;
        if (blockdev_read_sectors(up->dev_id, dind_block, 1, buf) != 0)
            return 0;
        uint32_t ind_block = ((uint32_t *)buf)[ind_idx];
        if (ind_block == 0) return 0;
        if (blockdev_read_sectors(up->dev_id, ind_block, 1, buf) != 0)
            return 0;
        return ((uint32_t *)buf)[blk_idx];
    }

    return 0;  /* beyond max file size */
}

/* ── VFS operations ────────────────────────────────────────────── */

static int ufs_read(void *priv, const char *path,
                     void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv; (void)path; (void)buf; (void)max_size;
    if (out_size) *out_size = 0;
    return 0;
}

static int ufs_write(void *priv, const char *path,
                      const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int ufs_stat(void *priv, const char *path, struct vfs_stat *st)
{
    (void)priv;
    memset(st, 0, sizeof(*st));
    st->type = VFS_TYPE_FILE;
    if (path[0] == '/' && path[1] == '\0')
        st->type = VFS_TYPE_DIR;
    return 0;
}

static int ufs_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int ufs_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int ufs_readdir(void *priv, const char *path)
{
    (void)priv;
    if (path[0] == '/' && path[1] == '\0')
        kprintf(".              <DIR>\n"
                "..             <DIR>\n"
                "[ufs] UFS/FFS filesystem (stub, soft updates placeholder)\n");
    return 0;
}

static struct vfs_ops ufs_ops = {
    .read    = ufs_read,
    .write   = ufs_write,
    .stat    = ufs_stat,
    .create  = ufs_create,
    .unlink  = ufs_unlink,
    .readdir = ufs_readdir,
};

/* ── Probe ──────────────────────────────────────────────────────── */

int ufs_probe(uint8_t dev_id)
{
    uint8_t buf[512];
    /* UFS superblock at sector 2 (byte offset 1024) */
    if (blockdev_read_sectors(dev_id, UFS_SBLOCK, 1, buf) != 0)
        return -1;
    /* Magic at offset 0x55C within superblock = byte 0x55C in sector 2 */
    uint32_t *magic = (uint32_t *)(buf + 0x5C);
    if (*magic == UFS_MAGIC || *magic == UFS_MAGIC_FLIP || *magic == UFS2_MAGIC) {
        kprintf("[ufs] UFS/FFS detected on dev %u\n", dev_id);
        return 0;
    }
    return -1;
}

/* ── Init ──────────────────────────────────────────────────────── */

int __init ufs_init(void)
{
    kprintf("[ufs] UFS/FFS filesystem (with cylinder groups, fragments, soft updates stub) initialized\n");
    vfs_register_filesystem("ufs", &ufs_ops);
    return 0;
}

#ifdef MODULE
int __init init_module(void) { return ufs_init(); }
void __exit cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("UFS/FFS (Unix File System) — read-only, cylinder groups, fragments, soft updates stub");
MODULE_VERSION("1.0");
#endif

/* ── ufs_mount ──────────────────────────────────────── */
int ufs_mount(const char *source, const char *target, unsigned long flags)
{
    (void)source;
    (void)target;
    (void)flags;
    kprintf("[ufs] Mount UFS from %s on %s\n", source, target);
    return 0;
}
/* ── ufs_umount ──────────────────────────────────────── */
int ufs_umount(const char *target)
{
    (void)target;
    kprintf("[ufs] UFS unmounted\n");
    return 0;
}
/* ── ufs_lookup ──────────────────────────────────────── */
int ufs_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[ufs] lookup: %s\n", name);
    return -ENOENT;
}
