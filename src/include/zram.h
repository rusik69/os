#ifndef ZRAM_H
#define ZRAM_H

#include "types.h"

/* ZRAM — compressed RAM block device */

/* Maximum number of ZRAM devices */
#define ZRAM_MAX_DEVICES 1

/* Default ZRAM device size (64 MB) */
#define ZRAM_DEFAULT_SIZE (64 * 1024 * 1024)

/* ZRAM compression algorithms */
#define ZRAM_COMP_LZO    0

/* Initialize ZRAM subsystem */
void zram_init(void);

/* Create a ZRAM device */
int zram_create_device(uint64_t disk_size);

/* Read/write ZRAM sectors (called by blockdev layer) */
int zram_read_sectors(uint64_t sector, void *buf, uint32_t count);
int zram_write_sectors(uint64_t sector, const void *buf, uint32_t count);

/* Get ZRAM stats */
uint64_t zram_get_compressed_size(void);
uint64_t zram_get_orig_size(void);
uint64_t zram_get_stored_pages(void);

#endif /* ZRAM_H */
