#ifndef FLOPPY_H
#define FLOPPY_H

#include "types.h"

/* Initialise the floppy disk controller.
   Returns 0 if a floppy controller is present and initialised, -1 otherwise. */
int floppy_init(void);

/* Check if a floppy drive is present. Returns 1 if present, 0 otherwise. */
int floppy_is_present(void);

/* Read sectors from floppy disk.
   drive: 0 for A:, 1 for B:
   lba: sector number
   count: number of sectors (max 256)
   buf: output buffer (must be count * 512 bytes)
   Returns number of sectors read on success, -1 on error. */
int floppy_read_sectors(int drive, uint32_t lba, uint8_t count, void *buf);

#endif /* FLOPPY_H */
