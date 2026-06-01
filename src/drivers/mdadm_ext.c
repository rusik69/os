#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "mdadm.h"
#include "string.h"

/* ── RAID0 set-bit (mark array as RAID0 in superblock) ───────────────── */

/* Set the RAID level to RAID0 in a superblock */
int raid_set_level_raid0(struct raid_super *super) {
    if (!super) return -EINVAL;
    super->level = 0;
    super->checksum = raid_super_checksum(super);
    kprintf("[mdadm] RAID superblock set to RAID0\n");
    return 0;
}

/* Check if a superblock is RAID0 */
int raid_is_raid0(const struct raid_super *super) {
    if (!super) return 0;
    return (super->magic == RAID_SUPER_MAGIC && super->level == 0);
}

/* Create a RAID0 superblock */
int raid_create_raid0(struct raid_super *super, uint32_t num_disks,
                      uint32_t chunk_size, uint64_t disk_sectors,
                      const uint8_t *uuid) {
    if (!super || !uuid) return -EINVAL;
    
    memset(super, 0, sizeof(*super));
    super->magic = RAID_SUPER_MAGIC;
    super->level = 0;
    super->num_disks = num_disks;
    super->chunk_size = chunk_size;
    super->disk_sectors = disk_sectors;
    memcpy(super->uuid, uuid, 16);
    super->checksum = raid_super_checksum(super);
    
    kprintf("[mdadm] RAID0 created: %u disks, chunk=%u sectors\n",
            num_disks, chunk_size);
    return 0;
}
