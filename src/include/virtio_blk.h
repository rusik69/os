#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "types.h"

/* Initialize virtio-blk (PCI 1AF4:1001).  Returns 0 if found, -1 if absent. */
int  virtio_blk_init(void);
/* Read/write full 512-byte sectors.  Returns 0 on success, -1 on error. */
int  virtio_blk_read_sectors(uint64_t lba, uint32_t count, void *buf);
int  virtio_blk_write_sectors(uint64_t lba, uint32_t count, const void *buf);
/* Returns total sector count, or 0 if device absent. */
uint64_t virtio_blk_sector_count(void);

#endif
