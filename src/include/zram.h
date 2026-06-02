#ifndef ZRAM_H
#define ZRAM_H

#include "types.h"
#include "zcomp.h"

/* ZRAM — compressed RAM block device */

/* Maximum number of ZRAM devices */
#define ZRAM_MAX_DEVICES 1

/* Default ZRAM device size (64 MB) */
#define ZRAM_DEFAULT_SIZE (64 * 1024 * 1024)

/* ZRAM compression algorithm identifiers (deprecated, use ZCOMP_ALGO_*) */
#define ZRAM_COMP_FAST   ZCOMP_ALGO_FAST
#define ZRAM_COMP_LZSS   ZCOMP_ALGO_LZSS
#define ZRAM_COMP_NONE   ZCOMP_ALGO_NONE

/* Maximum number of per-CPU streams (must match SMP_MAX_CPUS) */
#define ZRAM_MAX_STREAMS 16

/* Initialize ZRAM subsystem */
void zram_init(void);

/* Create a ZRAM device with specified compression algorithm */
int zram_create_device(uint64_t disk_size, uint32_t algo_id);

/* Create a ZRAM device with default algorithm (FAST) */
int zram_create_device_default(uint64_t disk_size);

/* Read/write ZRAM sectors (called by blockdev layer) */
int zram_read_sectors(uint64_t sector, void *buf, uint32_t count);
int zram_write_sectors(uint64_t sector, const void *buf, uint32_t count);

/* Get ZRAM stats */
uint64_t zram_get_compressed_size(void);
uint64_t zram_get_orig_size(void);
uint64_t zram_get_stored_pages(void);

/* Get/set compression algorithm (0 = FAST, 1 = LZSS, 2 = NONE) */
int zram_set_algorithm(uint32_t algo_id);
uint32_t zram_get_algorithm(void);

#endif /* ZRAM_H */
