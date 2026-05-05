#ifndef AHCI_H
#define AHCI_H

#include "types.h"

#define AHCI_SECTOR_SIZE 512

/* AHCI signature constants */
#define AHCI_SIG_ATA    0x00000101  /* SATA drive */
#define AHCI_SIG_ATAPI  0xEB140101  /* SATAPI drive */

int  ahci_init(void);
int  ahci_is_present(void);
uint32_t ahci_get_sectors(void);
int  ahci_read_sectors(uint32_t lba, uint8_t count, void *buf);
int  ahci_write_sectors(uint32_t lba, uint8_t count, const void *buf);

#endif
