#include "mdadm.h"
#include "string.h"
#include "printf.h"

uint32_t raid_super_checksum(const struct raid_super *super) {
    uint32_t sum = 0;
    const uint8_t *p = (const uint8_t *)super;
    /* Checksum includes fields before 'checksum' */
    size_t len = __builtin_offsetof(struct raid_super, checksum);
    for (size_t i = 0; i < len; i++) {
        sum ^= p[i];
        sum = (sum << 1) | (sum >> 31);  /* rotate left 1 */
    }
    return sum;
}

int raid_write_super(uint8_t *buffer, uint64_t sector_offset,
                     uint32_t level, uint32_t num_disks,
                     uint32_t chunk_size, uint64_t disk_sectors,
                     const uint8_t *uuid) {
    if (!buffer) return -1;

    /* Place superblock at byte offset within the buffer */
    struct raid_super *super = (struct raid_super *)(buffer + sector_offset * 512);
    if (!super) return -1;

    memset(super, 0, sizeof(*super));
    super->magic = RAID_SUPER_MAGIC;
    super->level = level;
    super->num_disks = num_disks;
    super->chunk_size = chunk_size;
    super->disk_sectors = disk_sectors;

    /* Copy UUID if provided */
    if (uuid) {
        memcpy(super->uuid, uuid, 16);
    }

    /* Compute checksum */
    super->checksum = raid_super_checksum(super);

    return 0;
}

int raid_read_super(const uint8_t *buffer, uint64_t sector_offset,
                    struct raid_super *super) {
    if (!buffer || !super) return -1;

    const struct raid_super *sb =
        (const struct raid_super *)(buffer + sector_offset * 512);
    if (!sb) return -1;

    /* Check magic */
    if (sb->magic != RAID_SUPER_MAGIC) {
        kprintf("[--] RAID superblock: bad magic 0x%x (expected 0x%x)\n",
                (uint32_t)sb->magic, (uint32_t)RAID_SUPER_MAGIC);
        return -1;
    }

    /* Validate checksum */
    uint32_t stored_checksum = sb->checksum;
    /* Temporarily zero checksum for validation */
    struct raid_super tmp = *sb;
    tmp.checksum = 0;
    /* Recompute */
    uint32_t computed = raid_super_checksum(&tmp);
    if (computed != stored_checksum) {
        kprintf("[--] RAID superblock: bad checksum 0x%x (expected 0x%x)\n",
                (uint32_t)stored_checksum, (uint32_t)computed);
        return -1;
    }

    /* Copy out the superblock */
    memcpy(super, sb, sizeof(*super));
    return 0;
}

/* ── Stub: mdadm_init ─────────────────────────────── */
int mdadm_init(void)
{
    kprintf("[mdadm] mdadm_init: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: mdadm_run_array ─────────────────────────────── */
int mdadm_run_array(const char *dev, int level, int disks)
{
    (void)dev;
    (void)level;
    (void)disks;
    kprintf("[mdadm] mdadm_run_array: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: mdadm_stop_array ─────────────────────────────── */
int mdadm_stop_array(const char *dev)
{
    (void)dev;
    kprintf("[mdadm] mdadm_stop_array: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: mdadm_add_disk ─────────────────────────────── */
int mdadm_add_disk(const char *dev, const char *disk)
{
    (void)dev;
    (void)disk;
    kprintf("[mdadm] mdadm_add_disk: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: mdadm_remove_disk ─────────────────────────────── */
int mdadm_remove_disk(const char *dev, const char *disk)
{
    (void)dev;
    (void)disk;
    kprintf("[mdadm] mdadm_remove_disk: not yet implemented\n");
    return -ENOSYS;
}
