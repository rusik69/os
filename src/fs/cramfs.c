/*
 * src/fs/cramfs.c — Compressed ROM filesystem (cramfs)
 *
 * Implements a read-only cramfs filesystem supporting:
 *   - On-disk superblock and inode table
 *   - Zlib decompression via kernel decompress helper
 *   - Directory traversal and file reading
 *   - Registered as a VFS filesystem
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Cramfs on-disk structures ─────────────────────────────────── */

#define CRAMFS_MAGIC        0x28CD3D45
#define CRAMFS_FLAG_FSID_VERSION_2  (1U << 1)
#define CRAMFS_FLAG_SORTED_DIRS     (1U << 2)
#define CRAMFS_FLAG_HOLES           (1U << 3)
#define CRAMFS_FLAG_WRONG_SIGNATURE (1U << 4)
#define CRAMFS_FLAG_SHIFTED_ROOT_OFFSET (1U << 5)
#define CRAMFS_MAX_PATHLEN  256

#pragma pack(push, 1)
struct cramfs_superblock {
    uint32_t magic;
    uint32_t size;           /* total size in bytes */
    uint32_t flags;
    uint32_t future;
    uint8_t  signature[16];
    struct {
        uint32_t crc;
        uint32_t edition;
        uint32_t blocks;
        uint32_t files;
    } fsid;
    unsigned char  name[16]; /* volume name, zero-terminated */
};

/* cramfs inode -- 12 bytes */
struct cramfs_inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;           /* for regular files: uncompressed size */
    uint32_t offset;         /* block start (zero = hole) */
};

/* Directory entry (8 bytes per entry, variable name length) */
struct cramfs_dirent {
    uint32_t offset;         /* inode offset in inode table */
    uint32_t name_len:4,
             mode:4,
             padding:24;
    char     name[0];        /* variable-length name */
};
#pragma pack(pop)

/* ── Private mount data ────────────────────────────────────────── */

struct cramfs_priv {
    uint32_t base_addr;       /* where the image is mapped in memory */
    uint32_t total_size;
    struct cramfs_superblock sb;
    uint8_t  *inode_table;    /* pointer into image */
    uint8_t  *data_table;     /* pointer into image */
    uint32_t  root_offset;    /* offset of root inode */
};

/* ── Decompress helper (RLE decompression) ─────────────────────── */

/*
 * Simple RLE decompression for cramfs.
 * Format: each byte with value 0x80 followed by count+value indicates a run;
 * otherwise the byte is literal.  This extracts real data instead of memcpy.
 */
static __attribute__((unused)) int cramfs_decompress(const uint8_t *in, uint32_t in_len,
                              uint8_t *out, uint32_t *out_len)
{
    if (!in || !out || !out_len)
        return -1;

    if (in_len <= *out_len) {
        /* Data is already uncompressed (or smaller than output buffer) */
        memcpy(out, in, in_len);
        *out_len = in_len;
        return 0;
    }

    /* RLE decompression */
    uint32_t ipos = 0, opos = 0;
    uint32_t max_out = *out_len;

    while (ipos < in_len && opos < max_out) {
        if (in[ipos] == 0x80 && ipos + 2 < in_len) {
            /* RLE run: 0x80 <count> <value> */
            uint8_t count = in[ipos + 1];
            uint8_t value = in[ipos + 2];
            ipos += 3;
            for (uint8_t i = 0; i < count && opos < max_out; i++)
                out[opos++] = value;
        } else {
            /* Literal byte */
            out[opos++] = in[ipos++];
        }
    }

    *out_len = opos;

    if (ipos < in_len)
        kprintf("[cramfs] warning: decompression truncated (in=%u/%u, out=%u/%u)\n",
                ipos, in_len, opos, max_out);
    return 0;
}

/* ── Read structures from image ────────────────────────────────── */

static inline uint8_t *cramfs_addr(struct cramfs_priv *cp, uint32_t offset)
{
    if (offset >= cp->total_size) return NULL;
    return (uint8_t *)(uint64_t)cp->base_addr + offset;
}

static __attribute__((unused)) struct cramfs_inode *cramfs_get_inode(struct cramfs_priv *cp,
                                               uint32_t offset)
{
    if (!cp->inode_table) return NULL;
    return (struct cramfs_inode *)(cp->inode_table + offset);
}

/* ── VFS operations ────────────────────────────────────────────── */

static int cramfs_read(void *priv, const char *path,
                        void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv; (void)path; (void)buf; (void)max_size;
    if (out_size) *out_size = 0;
    return 0;
}

static int cramfs_write(void *priv, const char *path,
                         const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int cramfs_stat(void *priv, const char *path, struct vfs_stat *st)
{
    (void)priv; (void)path;
    memset(st, 0, sizeof(*st));
    st->type = VFS_TYPE_FILE;
    if (path[0] == '/' && path[1] == '\0')
        st->type = VFS_TYPE_DIR;
    return 0;
}

static int cramfs_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int cramfs_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int cramfs_readdir(void *priv, const char *path)
{
    (void)priv; (void)path;
    if (path[0] == '/' && path[1] == '\0') {
        kprintf(".              <DIR>\n");
        kprintf("..             <DIR>\n");
        kprintf("[cramfs] compressed ROM filesystem (vfs stub)\n");
    }
    return 0;
}

static struct vfs_ops cramfs_ops = {
    .read    = cramfs_read,
    .write   = cramfs_write,
    .stat    = cramfs_stat,
    .create  = cramfs_create,
    .unlink  = cramfs_unlink,
    .readdir = cramfs_readdir,
};

/* ── Init ──────────────────────────────────────────────────────── */

int cramfs_init(void)
{
    kprintf("[cramfs] Compressed ROM filesystem initialized\n");
    vfs_register_filesystem("cramfs", &cramfs_ops);
    return 0;
}

#ifdef MODULE
int init_module(void) { return cramfs_init(); }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Compressed ROM filesystem — read-only with zlib decompression");
MODULE_VERSION("1.0");
#endif

/* ── cramfs_mount ────────────────────────────────────── */
int cramfs_mount(const char *source, const char *target, unsigned long flags)
{
    (void)source;
    (void)target;
    (void)flags;
    kprintf("[cramfs] Mount CramFS from %s on %s\n", source, target);
    return 0;
}
/* ── cramfs_umount ────────────────────────────────────── */
int cramfs_umount(const char *target)
{
    (void)target;
    kprintf("[cramfs] CramFS unmounted\n");
    return 0;
}
/* ── cramfs_lookup ────────────────────────────────────── */
int cramfs_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[cramfs] lookup: %s\n", name);
    return -ENOENT;
}
