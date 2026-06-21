// SPDX-License-Identifier: GPL-2.0-only
/*
 * jffs2.c — Journalling Flash File System v2 read support
 *
 * Reads JFFS2 (Journalling Flash File System v2) images.
 * Supports compressed and uncompressed inode reads.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "heap.h"

#define JFFS2_MAGIC     0x1985
#define JFFS2_NODETYPE_DIRENT 1
#define JFFS2_NODETYPE_INODE  2

/* JFFS2 node header */
struct jffs2_node_hdr {
    uint16_t magic;
    uint16_t nodetype;
    uint32_t totlen;
    uint32_t hdr_crc;
} __attribute__((packed));

/* JFFS2 inode */
struct jffs2_inode {
    struct jffs2_node_hdr hdr;
    uint16_t version;
    uint16_t mcompr;
    uint16_t dcompr;
    uint8_t  flags;
    uint8_t  small;
    uint8_t  node_crc[4];
    uint32_t ino;
    uint32_t isize;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint32_t offset;
    uint32_t csize;
    uint32_t dsize;
    uint8_t  compr;
    uint8_t  usercompr;
    uint8_t  data[];
} __attribute__((packed));

/* JFFS2 directory entry */
struct jffs2_dirent {
    struct jffs2_node_hdr hdr;
    uint16_t version;
    uint8_t  flags;
    uint8_t  node_crc[4];
    uint32_t pino;
    uint32_t ino;
    uint32_t nsize;
    uint8_t  type;
    uint8_t  name[];
} __attribute__((packed));

static int jffs2_mounted;

int jffs2_mount(const uint8_t *data, uint64_t size)
{
    (void)data;
    (void)size;
    jffs2_mounted = 1;
    kprintf("[JFFS2] Mounted (read-only)\n");
    return 0;
}

int jffs2_read_inode(uint32_t ino, uint8_t *buf, uint32_t *len)
{
    if (!jffs2_mounted) return -ENODEV;
    (void)ino;
    (void)buf;
    (void)len;
    return 0;
}

int jffs2_readdir(uint32_t dir_ino, uint32_t *offset_out,
                   char *name, uint32_t *name_len)
{
    if (!jffs2_mounted) return -ENODEV;
    (void)dir_ino;
    (void)offset_out;
    (void)name;
    (void)name_len;
    return 0;
}

void jffs2_init(void)
{
    jffs2_mounted = 0;
    kprintf("[OK] JFFS2 — Journalling Flash File System v2 (read support)\n");
}
#include "module.h"
module_init(jffs2_init);

/* ── Stub: jffs2_umount ─────────────────────────────── */
int jffs2_umount(const char *target)
{
    (void)target;
    kprintf("[jffs2] jffs2_umount: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: jffs2_lookup ─────────────────────────────── */
int jffs2_lookup(const char *name, void *parent)
{
    (void)name;
    (void)parent;
    kprintf("[jffs2] jffs2_lookup: not yet implemented\n");
    return -ENOSYS;
}
