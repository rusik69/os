#ifndef MDADM_H
#define MDADM_H

#include "types.h"

#define RAID_SUPER_MAGIC 0xA0B0C0D0

/* RAID superblock (placed at end of each disk in the array) */
struct raid_super {
    uint32_t magic;         /* RAID_SUPER_MAGIC */
    uint32_t level;         /* RAID level (0 = RAID0) */
    uint32_t num_disks;     /* number of member disks */
    uint32_t chunk_size;    /* stripe chunk size in sectors */
    uint64_t disk_sectors;  /* total sectors on this disk */
    uint8_t  uuid[16];      /* array UUID (unique per array) */
    uint32_t checksum;      /* simple XOR checksum of preceding fields */
} __attribute__((packed));

/* Write RAID superblock to a buffer at the given sector offset (end of disk).
   Returns 0 on success, -1 on error. */
int raid_write_super(uint8_t *buffer, uint64_t sector_offset,
                     uint32_t level, uint32_t num_disks,
                     uint32_t chunk_size, uint64_t disk_sectors,
                     const uint8_t *uuid);

/* Read and validate a RAID superblock from a buffer.
   Returns 0 on success, -1 on error (bad magic/checksum). */
int raid_read_super(const uint8_t *buffer, uint64_t sector_offset,
                    struct raid_super *super);

/* Compute the simple XOR checksum of a superblock. */
uint32_t raid_super_checksum(const struct raid_super *super);

#endif /* MDADM_H */
