#ifndef RAMDISK_H
#define RAMDISK_H

#include "types.h"

/* Ramdisk: memory-backed block device for testing filesystem code
 * without ATA hardware (or without slow ATA PIO emulation in TCG).
 *
 * All storage is allocated from the kernel heap in 4 KB pages.
 * Default size: RAMDISK_SIZE sectors × 512 bytes.
 */

#define RAMDISK_SECTOR_SIZE  512
#define RAMDISK_SECTORS      4096   /* 2 MB ramdisk */

void     ramdisk_init(void);
int      ramdisk_read_sectors(uint32_t lba, uint8_t count, void *buf);
int      ramdisk_write_sectors(uint32_t lba, uint8_t count, const void *buf);
int      ramdisk_is_present(void);
uint32_t ramdisk_get_sectors(void);

#endif
