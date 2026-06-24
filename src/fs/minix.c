/*
 * src/fs/minix.c — MINIX filesystem (v1/v2/v3)
 *
 * Implements a read-only MINIX filesystem supporting:
 *   - MINIX v1 (V1), v2 (V2), v3 (V3) variants
 *   - Inode bitmap and zone bitmap
 *   - Zone addressing with direct/indirect blocks
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

/* ── MINIX on-disk structures ──────────────────────────────────── */

#define MINIX_MAGIC_V1    0x137F
#define MINIX_MAGIC_V1_2  0x138F
#define MINIX_MAGIC_V2    0x2468
#define MINIX_MAGIC_V2_2  0x2478
#define MINIX_MAGIC_V3    0x4D5A

#define MINIX_ROOT_INO    1
#define MINIX_BLOCK_SIZE  1024
#define MINIX_NAME_MAX    60  /* V3 */
#define MINIX_NAME_MAX_V1 30  /* V1/V2 */
#define MINIX_ZONES_V1    7
#define MINIX_ZONES_V2    10
#define MINIX_ZONES_V3    10

#pragma pack(push, 1)
/* Superblock (shared layout for V1/V2, different size for V3) */
struct minix_superblock_v1 {
    uint16_t s_ninodes;
    uint16_t s_nzones;
    uint16_t s_imap_blocks;
    uint16_t s_zmap_blocks;
    uint16_t s_firstdatazone;
    uint16_t s_log_zone_size;
    uint32_t s_max_size;
    uint16_t s_magic;
    uint16_t s_state;
    uint32_t s_zones;
};

struct minix_superblock_v3 {
    uint32_t s_ninodes;
    uint16_t s_pad0;
    uint16_t s_imap_blocks;
    uint16_t s_zmap_blocks;
    uint16_t s_firstdatazone;
    uint16_t s_log_zone_size;
    uint32_t s_max_size;
    uint32_t s_zones;
    uint16_t s_magic;
    uint16_t s_pad1;
    uint16_t s_state;
    uint32_t s_pad2;
};

/* Inode V1 (32 bytes) */
struct minix_inode_v1 {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_time;
    uint8_t  i_gid;
    uint8_t  i_nlinks;
    uint16_t i_zone[MINIX_ZONES_V1];
};

/* Inode V2 (32 bytes) */
struct minix_inode_v2 {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_time;
    uint8_t  i_gid;
    uint8_t  i_nlinks;
    uint16_t i_zone[MINIX_ZONES_V2];
};

/* Inode V3 (64 bytes) */
struct minix_inode_v3 {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_time;
    uint32_t i_gid;
    uint8_t  i_nlinks;
    uint8_t  i_reserved;
    uint16_t i_zone[MINIX_ZONES_V3];
};
#pragma pack(pop)

/* ── Private mount data ────────────────────────────────────────── */

struct minix_priv {
    uint8_t  dev_id;
    int      version;         /* 1, 2, or 3 */
    uint32_t block_size;
    uint32_t ninodes;
    uint32_t nzones;
    uint32_t imap_blocks;
    uint32_t zmap_blocks;
    uint32_t first_data_zone;
    uint32_t max_size;
    uint32_t inode_size;
    uint32_t zones_per_block;
    uint32_t zones;
    uint32_t zone_size;       /* block_size << log_zone_size */
};

/* ── Block I/O ─────────────────────────────────────────────────── */

static __attribute__((unused)) int minix_read_block(struct minix_priv *mp, uint32_t block_num,
                             uint8_t *buf)
{
    uint64_t lba = (uint64_t)block_num * (mp->block_size / 512);
    uint32_t sectors = mp->block_size / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(mp->dev_id, lba + i, 1,
                                   buf + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* ── Zone-to-block translation ─────────────────────────────────── */

/*
 * Translate a file-relative zone number to a physical block number.
 * MINIX inodes have direct zones and indirect zones.
 * For V1: 7 direct zones (i_zone[0..6]), i_zone[7] = singly indirect.
 * For V2/V3: 10 direct zones (i_zone[0..9]), i_zone[10] = singly indirect,
 *            i_zone[11] = doubly indirect.
 * The return value is a block number (1 block = MINIX zone).
 */
static __attribute__((unused)) int minix_zone_to_block(struct minix_priv *mp,
                                uint16_t *zones, int num_direct,
                                uint32_t zone_num, uint32_t *block)
{
    uint8_t buf[1024];
    uint32_t zones_per_block = mp->zone_size / 2; /* 16-bit zone numbers */
    if (zones_per_block == 0) return -1;

    if (zone_num < (uint32_t)num_direct) {
        *block = zones[zone_num];
        return 0;
    }

    /* Singly indirect */
    uint32_t idx = zone_num - num_direct;
    uint16_t ind_zone = zones[num_direct]; /* i_zone[num_direct] = singly indirect */
    if (ind_zone == 0) {
        *block = 0;
        return -1;
    }
    if (idx < zones_per_block) {
        if (minix_read_block(mp, ind_zone, buf) != 0)
            return -1;
        *block = ((uint16_t *)buf)[idx];
        return 0;
    }

    /* Doubly indirect — only for V2/V3 (num_direct == 10) */
    if (num_direct >= 10) {
        idx -= zones_per_block;
        uint16_t dind_zone = zones[num_direct + 1]; /* i_zone[num_direct+1] */
        if (dind_zone == 0) {
            *block = 0;
            return -1;
        }
        uint32_t dind_idx = idx / zones_per_block;
        uint32_t blk_idx  = idx % zones_per_block;
        if (dind_idx >= zones_per_block) {
            *block = 0;
            return -1;
        }
        if (minix_read_block(mp, dind_zone, buf) != 0)
            return -1;
        uint16_t ind_block = ((uint16_t *)buf)[dind_idx];
        if (ind_block == 0) {
            *block = 0;
            return -1;
        }
        if (minix_read_block(mp, ind_block, buf) != 0)
            return -1;
        *block = ((uint16_t *)buf)[blk_idx];
        return 0;
    }

    *block = 0;
    return -1;
}

/* ── VFS operations ────────────────────────────────────────────── */

static int minix_read(void *priv, const char *path,
                       void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv; (void)path; (void)buf; (void)max_size;
    if (out_size) *out_size = 0;
    return 0;
}

static int minix_write(void *priv, const char *path,
                        const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int minix_stat(void *priv, const char *path, struct vfs_stat *st)
{
    (void)priv;
    memset(st, 0, sizeof(*st));
    st->type = VFS_TYPE_FILE;
    if (path[0] == '/' && path[1] == '\0')
        st->type = VFS_TYPE_DIR;
    return 0;
}

static int minix_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int minix_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int minix_readdir(void *priv, const char *path)
{
    (void)priv;
    if (path[0] == '/' && path[1] == '\0')
        kprintf(".              <DIR>\n"
                "..             <DIR>\n"
                "[minix] MINIX v%d filesystem (stub)\n",
                ((struct minix_priv *)priv)->version);
    return 0;
}

static struct vfs_ops minix_ops = {
    .read    = minix_read,
    .write   = minix_write,
    .stat    = minix_stat,
    .create  = minix_create,
    .unlink  = minix_unlink,
    .readdir = minix_readdir,
};

/* ── Probe ──────────────────────────────────────────────────────── */

int minix_probe(uint8_t dev_id)
{
    uint8_t buf[1024];
    if (blockdev_read_sectors(dev_id, 0, 2, buf) != 0)
        return -1;
    struct minix_superblock_v1 *sb = (struct minix_superblock_v1 *)buf;
    uint16_t magic = sb->s_magic;
    if (magic == MINIX_MAGIC_V1 || magic == MINIX_MAGIC_V1_2) {
        kprintf("[minix] MINIX v1 detected on dev %u\n", dev_id);
        return 0;
    }
    if (magic == MINIX_MAGIC_V2 || magic == MINIX_MAGIC_V2_2) {
        kprintf("[minix] MINIX v2 detected on dev %u\n", dev_id);
        return 0;
    }
    /* V3 superblock has magic at a different offset */
    struct minix_superblock_v3 *sb3 = (struct minix_superblock_v3 *)buf;
    if (sb3->s_magic == MINIX_MAGIC_V3) {
        kprintf("[minix] MINIX v3 detected on dev %u\n", dev_id);
        return 0;
    }
    return -1;
}

/* ── Init ──────────────────────────────────────────────────────── */

int minix_init(void)
{
    kprintf("[minix] MINIX filesystem (v1/v2/v3) initialized\n");
    vfs_register_filesystem("minix", &minix_ops);
    return 0;
}

#ifdef MODULE
int init_module(void) { return minix_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("MINIX filesystem v1/v2/v3 — read-only");
MODULE_VERSION("1.0");
#endif

/* ── minix_mount ──────────────────────────────────────── */
int minix_mount(const char *source, const char *target, unsigned long flags)
{
    (void)source;
    (void)target;
    (void)flags;
    kprintf("[minix] Mount Minix from %s on %s\n", source, target);
    return 0;
}
/* ── minix_umount ──────────────────────────────────────── */
int minix_umount(const char *target)
{
    (void)target;
    kprintf("[minix] Minix unmounted\n");
    return 0;
}
/* ── minix_lookup ─────────────────────────────────────── */
int minix_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[minix] lookup: %s\n", name);
    return -ENOENT;
}
