#ifndef PARTITIONS_H
#define PARTITIONS_H

#include "types.h"

/* MBR partition table entry (16 bytes) */
struct partition_entry {
    uint8_t  bootable;       /* 0x80 = bootable */
    uint8_t  start_head;
    uint8_t  start_sector;   /* bits 0-5: sector, bits 6-7: high bits of cylinder */
    uint8_t  start_cyl;
    uint8_t  type;           /* partition type (e.g. 0x83 = Linux) */
    uint8_t  end_head;
    uint8_t  end_sector;
    uint8_t  end_cyl;
    uint32_t start_lba;      /* LBA of first sector */
    uint32_t sector_count;   /* number of sectors */
} __attribute__((packed));

/* MBR structure (sector 0, 512 bytes) */
struct mbr {
    uint8_t  bootstrap[446];
    struct partition_entry partitions[4];
    uint16_t signature;      /* must be 0xAA55 */
} __attribute__((packed));

/* Parse MBR from a 512-byte buffer. Fills entries array (up to 4).
   Returns number of valid (non-zero type) partitions found. */
int partitions_read(const uint8_t *mbr_sector, struct partition_entry *entries);

/* Get a human-readable name for a partition type. */
const char *partition_type_name(uint8_t type);

#endif /* PARTITIONS_H */
