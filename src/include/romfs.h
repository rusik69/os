#ifndef ROMFS_H
#define ROMFS_H

#include "types.h"
#include "vfs.h"

/* ROMFS magic */
#define ROMFS_MAGIC 0x6D6F682D /* "-rom" (little-endian: "-rom") */
#define ROMFS_MAGIC_WORD "-rom1fs-"

/* ROMFS superblock (32 bytes) */
struct romfs_super {
    uint32_t magic;        /* "-rom" */
    uint32_t full_size;    /* total size of FS image */
    uint32_t checksum;
    char     volume_name[16];
} __attribute__((packed));

/* ROMFS file header (minimal) */
struct romfs_file {
    uint32_t next;        /* offset of next file (bit 0 = 1 if dir) */
    uint32_t spec_info;   /* for dir: first file; for symlink: target... */
    uint32_t size;
    uint32_t checksum;
    char     name[16];    /* variable length, null-terminated */
} __attribute__((packed));

#define ROMFS_NEXT_IS_DIR 1

int romfs_mount(const char *mountpoint, uint32_t addr, uint32_t size);
int romfs_init(void);

#endif /* ROMFS_H */
