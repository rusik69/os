#ifndef ATA_H
#define ATA_H

#include "types.h"

#define ATA_SECTOR_SIZE 512

void ata_init(void);
int ata_read_sectors(uint32_t lba, uint8_t count, void *buf);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);
int ata_is_present(void);
uint32_t ata_get_sectors(void);

#endif
