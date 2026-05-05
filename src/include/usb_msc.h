#ifndef USB_MSC_H
#define USB_MSC_H

/*
 * USB Mass Storage Class (MSC) — Bulk-Only Transport (BOT)
 *
 * Provides sector read/write for USB flash drives (FAT32).
 * After usb_msc_init() succeeds the device is registered as BLOCKDEV_USB0.
 */

#include "types.h"

/* Returns 0 on success (device found + registered), negative on error */
int usb_msc_init(void);

/* Low-level sector I/O (registered with blockdev layer) */
int      usb_msc_read_sectors (uint32_t lba, uint8_t count, void *buf);
int      usb_msc_write_sectors(uint32_t lba, uint8_t count, const void *buf);
uint32_t usb_msc_get_sectors  (void);

#endif
