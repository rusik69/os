#ifndef TARFS_H
#define TARFS_H

#include "types.h"
#include "vfs.h"

/* tar header (512 bytes) */
#define TAR_BLOCK_SIZE 512
#define TAR_MAGIC "ustar"
#define TAR_MAGIC_OLD "ustar  "

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed));

/* Type flags */
#define TAR_TYPE_FILE     '0'
#define TAR_TYPE_HARD_LINK '1'
#define TAR_TYPE_SYMLINK  '2'
#define TAR_TYPE_DIR      '5'

/* Mount a tar archive as a read-only filesystem from a memory buffer */
int tarfs_mount(const char *mountpoint, uint64_t addr, uint64_t size);
int tarfs_init(void);

#endif /* TARFS_H */
