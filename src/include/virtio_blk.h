#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "types.h"

/* Initialize virtio-blk (PCI 1AF4:1001).  Returns 0 if found, -1 if absent. */
int  virtio_blk_init(void);
/* Read/write full 512-byte sectors.  Returns 0 on success, -1 on error. */
int  virtio_blk_read_sectors(uint64_t lba, uint32_t count, void *buf);
int  virtio_blk_write_sectors(uint64_t lba, uint32_t count, const void *buf);
/* Discard/deallocate a range of sectors (TRIM).  Returns 0 on success, -1 on error. */
int  virtio_blk_discard_sectors(uint64_t lba, uint32_t count);
/* Write zeroes to a range of sectors.  Returns 0 on success, -1 on error. */
int  virtio_blk_write_zeroes_sectors(uint64_t lba, uint32_t count);
/* Returns total sector count, or 0 if device absent. */
uint64_t virtio_blk_sector_count(void);

/* ── Life-time / IOPS config getters ─────────────────────────── */
uint32_t virtio_blk_get_max_discard_sectors(void);
uint32_t virtio_blk_get_max_discard_seg(void);
uint32_t virtio_blk_get_discard_sector_alignment(void);
uint32_t virtio_blk_get_max_write_zeroes_sectors(void);
uint32_t virtio_blk_get_max_write_zeroes_seg(void);
uint8_t  virtio_blk_get_write_zeroes_may_unmap(void);
uint32_t virtio_blk_get_max_lifetime_discard_sectors(void);
uint32_t virtio_blk_get_max_segment_lifetime(void);
uint32_t virtio_blk_get_max_total_lifetime(void);
uint32_t virtio_blk_get_iops_max(void);
uint32_t virtio_blk_get_iops_min(void);
uint32_t virtio_blk_get_iops_wr_max(void);
uint32_t virtio_blk_get_iops_wr_min(void);

void virtio_blk_register_blockdev(void);

#endif
