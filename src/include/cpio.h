#ifndef CPIO_H
#define CPIO_H

#include "types.h"

/* CPIO newc header (110 bytes) */
struct cpio_newc_header {
    char c_magic[6];      /* "070701" */
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
} __attribute__((packed));

#define CPIO_MAGIC "070701"
#define CPIO_MAGIC_END "070701"
#define CPIO_TRAILER "TRAILER!!!"

/* Extract all files from a CPIO archive at the given memory address */
int cpio_extract_initramfs(uint32_t addr, uint32_t size);
int cpio_init(void);

#endif /* CPIO_H */
