#ifndef ATA_H
#define ATA_H

#include "types.h"

#define ATA_SECTOR_SIZE 512

void ata_init(void);
int ata_read_sectors(uint32_t lba, uint8_t count, void *buf);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);
int ata_is_present(void);
uint32_t ata_get_sectors(void);

/* Redirect ATA sector I/O to custom callbacks (e.g. ramdisk).
 * When set, ata_is_present() returns true and read/write go through
 * the callbacks instead of real ATA PIO.  Set both to NULL to restore. */
void ata_set_redirect(int (*read_fn)(uint32_t, uint8_t, void *),
                      int (*write_fn)(uint32_t, uint8_t, const void *));

#endif
