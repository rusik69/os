#ifndef FAT32_H
#define FAT32_H

#include "types.h"

#define FAT32_MAX_NAME 256

/* Mount point: which disk to use for FAT32 */
typedef enum {
    FAT32_DISK_ATA  = 0,
    FAT32_DISK_AHCI = 1,
    FAT32_DISK_USB0 = 2,
} fat32_disk_t;

int  fat32_mount(fat32_disk_t disk, uint32_t part_lba); /* 0 = auto-detect partition */
int  fat32_is_mounted(void);

/* Read a file; returns bytes read or negative on error */
int  fat32_read_file(const char *path, void *buf, uint32_t max_size);

/* List directory; names[] filled with up to max entries, returns count */
int  fat32_list_dir(const char *path, char names[][FAT32_MAX_NAME], int max);

/* Returns file size in bytes, or -1 if not found */
int  fat32_file_size(const char *path);

#endif
