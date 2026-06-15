/* cmd_blkid.c — Block device identification (S155)
 *
 * Scans a block device for known filesystem signatures and prints:
 *   UUID, LABEL, TYPE, PARTUUID
 *
 * Usage:
 *   blkid                    — show all devices
 *   blkid <device>           — scan specific device
 *   blkid -o value -s TYPE /dev/hda1  — output only value for given tag
 *
 * Detected filesystems:
 *   ext2/3/4  — superblock magic 0xEF53
 *   FAT12/16/32 — boot sector signature 0x55AA + BPB
 *   swap      — swap signature "SWAPSPACE2" or "SWAP-SPACE"
 *   XFS       — XFS superblock magic "XFSB"
 */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "blockdev.h"

/* ── Filesystem signature scanning ────────────────────────────────── */

struct blkid_fs_info {
    char type[16];
    char uuid[37];     /* UUID string (36 chars + null) */
    char label[32];
    char partuuid[37];
    int  detected;
};

/* Swap signature offsets */
#define SWAP_SIG1 "SWAPSPACE2"
#define SWAP_SIG2 "SWAP-SPACE"
#define SWAP_OFFSET 4086   /* at byte 4086 in page 0 */

/* XFS superblock magic */
#define XFS_MAGIC "XFSB"
#define XFS_OFFSET 0   /* at byte 0 of sector 0 */

/* Scan ext2/3/4 superblock (at byte offset 1024) */
static void scan_ext2(int dev_id, struct blkid_fs_info *info, uint64_t lba)
{
    uint8_t buf[1024];

    /* Read 2 sectors starting at LBA 2 (byte offset 1024) */
    if (blk_submit_sync(dev_id, lba + 2, 2, buf, BLK_REQ_READ) != 0)
        return;

    struct ext2_sb_scan {
        uint32_t inodes_count;
        uint32_t blocks_count;
        uint32_t r_blocks_count;
        uint32_t free_blocks_count;
        uint32_t free_inodes_count;
        uint32_t first_data_block;
        uint32_t log_block_size;
        uint32_t frag_size;
        uint32_t blocks_per_group;
        uint32_t frags_per_group;
        uint32_t inodes_per_group;
        uint32_t mtime;
        uint32_t wtime;
        uint16_t mnt_count;
        uint16_t max_mnt_count;
        uint16_t magic;
        uint16_t state;
        uint16_t errors;
        uint16_t minor_rev;
        uint32_t lastcheck;
        uint32_t checkinterval;
        uint32_t creator_os;
        uint32_t rev_level;
        uint16_t def_resuid;
        uint16_t def_resgid;
        uint32_t first_ino;
        uint16_t inode_size;
        uint16_t block_group_nr;
        uint32_t feature_compat;
        uint32_t feature_incompat;
        uint32_t feature_ro_compat;
        uint8_t  uuid[16];
        char     volume_name[16];
        char     last_mounted[64];
    } __attribute__((packed));

    struct ext2_sb_scan sb;
    memcpy(&sb, buf, sizeof(sb));

    if (sb.magic != 0xEF53)
        return;

    info->detected = 1;
    strncpy(info->type, "ext2", sizeof(info->type) - 1);
    info->type[sizeof(info->type) - 1] = '\0';

    /* Format UUID */
    snprintf(info->uuid, sizeof(info->uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             sb.uuid[0], sb.uuid[1], sb.uuid[2], sb.uuid[3],
             sb.uuid[4], sb.uuid[5], sb.uuid[6], sb.uuid[7],
             sb.uuid[8], sb.uuid[9], sb.uuid[10], sb.uuid[11],
             sb.uuid[12], sb.uuid[13], sb.uuid[14], sb.uuid[15]);

    /* Format label (may not be null-terminated) */
    int i;
    for (i = 0; i < 16 && sb.volume_name[i] && sb.volume_name[i] != ' '; i++)
        info->label[i] = sb.volume_name[i];
    info->label[i] = '\0';
    if (i == 0) { strncpy(info->label, "(none)", sizeof(info->label) - 1); info->label[sizeof(info->label) - 1] = '\0'; }
}

/* Scan FAT12/16/32 boot sector (at LBA 0) */
static void scan_fat(int dev_id, struct blkid_fs_info *info, uint64_t lba)
{
    uint8_t buf[512];
    if (blk_submit_sync(dev_id, lba, 1, buf, BLK_REQ_READ) != 0)
        return;

    /* Check boot signature 0x55AA */
    if (buf[510] != 0x55 || buf[511] != 0xAA)
        return;

    /* Check BPB signature 0x29 (extended boot signature) */
    if (buf[38] != 0x29 && buf[38] != 0x28)
        return;

    /* Determine FAT type */
    uint16_t bytes_per_sector = *(uint16_t *)(buf + 11);
    uint8_t  sectors_per_cluster = buf[13];
    uint16_t reserved_sectors = *(uint16_t *)(buf + 14);
    uint8_t  num_fats = buf[16];
    uint16_t root_entries = *(uint16_t *)(buf + 17);
    uint16_t total_sectors_16 = *(uint16_t *)(buf + 19);
    uint8_t  media = buf[21];
    uint16_t fat_size_16 = *(uint16_t *)(buf + 22);

    if (bytes_per_sector != 512)
        return;

    /* Calculate data sectors and total clusters */
    uint32_t total_sectors;
    if (total_sectors_16 != 0)
        total_sectors = total_sectors_16;
    else
        total_sectors = *(uint32_t *)(buf + 32);

    uint32_t fat_size = fat_size_16;
    if (fat_size == 0)
        fat_size = *(uint32_t *)(buf + 36);  /* FAT32 */

    uint32_t root_dir_sectors = (root_entries * 32 + bytes_per_sector - 1) / bytes_per_sector;
    uint32_t data_sectors = total_sectors - reserved_sectors - num_fats * fat_size - root_dir_sectors;
    uint32_t total_clusters = data_sectors / sectors_per_cluster;

    if (total_clusters < 4085)
        strncpy(info->type, "vfat", sizeof(info->type) - 1);
    else if (total_clusters < 65525)
        strncpy(info->type, "vfat", sizeof(info->type) - 1);
    else
        strncpy(info->type, "vfat", sizeof(info->type) - 1);
    info->type[sizeof(info->type) - 1] = '\0';

    info->detected = 1;

    /* Volume ID at offset 39 (4 bytes) */
    uint32_t vol_id = *(uint32_t *)(buf + 39);
    snprintf(info->uuid, sizeof(info->uuid), "%04X-%04X",
             (unsigned int)(vol_id >> 16), (unsigned int)(vol_id & 0xFFFF));

    /* Volume label at offset 43 (11 bytes) */
    int i;
    for (i = 0; i < 11 && buf[43 + i] && buf[43 + i] != ' '; i++)
        info->label[i] = buf[43 + i];
    info->label[i] = '\0';
    if (i == 0) { strncpy(info->label, "(none)", sizeof(info->label) - 1); info->label[sizeof(info->label) - 1] = '\0'; }
}

/* Scan swap signature */
static void scan_swap(int dev_id, struct blkid_fs_info *info, uint64_t lba)
{
    uint8_t buf[512];

    /* Swap signature is at offset 4086 within the first page (sectors 0-7) */
    uint32_t sector_offset = SWAP_OFFSET / 512;
    if (blk_submit_sync(dev_id, lba + sector_offset, 1, buf, BLK_REQ_READ) != 0)
        return;

    uint32_t byte_off = SWAP_OFFSET % 512;
    if (byte_off + 10 > 512) return;

    if (memcmp(buf + byte_off, SWAP_SIG1, 10) == 0 ||
        memcmp(buf + byte_off, SWAP_SIG2, 10) == 0) {
        info->detected = 1;
        strncpy(info->type, "swap", sizeof(info->type) - 1);
        info->type[sizeof(info->type) - 1] = '\0';
        strncpy(info->label, "(none)", sizeof(info->label) - 1);
        info->label[sizeof(info->label) - 1] = '\0';
        strncpy(info->uuid, "(none)", sizeof(info->uuid) - 1);
        info->uuid[sizeof(info->uuid) - 1] = '\0';
    }
}

/* Scan XFS superblock (at offset 0) */
static void scan_xfs(int dev_id, struct blkid_fs_info *info, uint64_t lba)
{
    uint8_t buf[512];
    if (blk_submit_sync(dev_id, lba, 1, buf, BLK_REQ_READ) != 0)
        return;

    /* XFS magic "XFSB" at offset 0 */
    if (memcmp(buf, XFS_MAGIC, 4) != 0)
        return;

    info->detected = 1;
    strncpy(info->type, "xfs", sizeof(info->type) - 1);
    info->type[sizeof(info->type) - 1] = '\0';

    /* UUID at offset 32 (16 bytes) */
    uint8_t *uuid = buf + 32;
    snprintf(info->uuid, sizeof(info->uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9], uuid[10], uuid[11],
             uuid[12], uuid[13], uuid[14], uuid[15]);

    /* Label at offset 108 (12 chars) */
    int i;
    for (i = 0; i < 12 && buf[108 + i] && buf[108 + i] != ' '; i++)
        info->label[i] = buf[108 + i];
    info->label[i] = '\0';
    if (i == 0) { strncpy(info->label, "(none)", sizeof(info->label) - 1); info->label[sizeof(info->label) - 1] = '\0'; }
}

/* ── Main command ──────────────────────────────────────────────────── */

void cmd_blkid(const char *args)
{
    char device[64] = "";
    int output_value = 0;
    char show_tag[32] = "";
    int tag_mode = 0;  /* 0 = show all, 1 = show specific tag */
    char *saveptr;
    char buf[256];

    /* Parse args */
    if (args && args[0]) {
        size_t alen = strlen(args);
        if (alen >= sizeof(buf)) alen = sizeof(buf) - 1;
        memcpy(buf, args, alen);
        buf[alen] = '\0';

        char *token = strtok_r(buf, " ", &saveptr);
        while (token) {
            if (strcmp(token, "-o") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val && strcmp(val, "value") == 0)
                    output_value = 1;
            } else if (strcmp(token, "-s") == 0) {
                char *val = strtok_r(NULL, " ", &saveptr);
                if (val) {
                    strncpy(show_tag, val, 31);
                    show_tag[31] = '\0';
                    tag_mode = 1;
                }
            } else if (token[0] != '-') {
                strncpy(device, token, 63);
                device[63] = '\0';
            }
            token = strtok_r(NULL, " ", &saveptr);
        }
    }

    /* Scan specific device or all devices */
    if (device[0]) {
        /* Scan named device */
        int dev_id = blockdev_find_by_name(device);
        if (dev_id < 0) {
            dev_id = atoi(device);
            if (dev_id < 0 || !blockdev_is_registered(dev_id)) {
                kprintf("blkid: cannot find device '%s'\n", device);
                return;
            }
        }

        struct blkid_fs_info info;
        memset(&info, 0, sizeof(info));

        /* Try each filesystem signature */
        scan_ext2(dev_id, &info, 0);
        if (!info.detected) scan_fat(dev_id, &info, 0);
        if (!info.detected) scan_swap(dev_id, &info, 0);
        if (!info.detected) scan_xfs(dev_id, &info, 0);

        /* Output results */
        const char *dev_name = blockdev_name(dev_id);
        if (!dev_name) dev_name = device;

        if (output_value && tag_mode) {
            if (strcmp(show_tag, "TYPE") == 0)
                kprintf("%s\n", info.detected ? info.type : "(unknown)");
            else if (strcmp(show_tag, "UUID") == 0)
                kprintf("%s\n", info.detected ? info.uuid : "(unknown)");
            else if (strcmp(show_tag, "LABEL") == 0)
                kprintf("%s\n", info.detected ? info.label : "(unknown)");
        } else if (info.detected) {
            kprintf("%s: TYPE=\"%s\" UUID=\"%s\" LABEL=\"%s\"\n",
                    dev_name, info.type, info.uuid, info.label);
        } else {
            kprintf("%s: TYPE=\"(unknown)\"\n", dev_name);
        }
    } else {
        /* Scan all registered block devices */
        int found_any = 0;
        for (int dev_id = 0; dev_id < BLOCKDEV_MAX_DEVICES; dev_id++) {
            if (!blockdev_is_registered(dev_id))
                continue;

            struct blkid_fs_info info;
            memset(&info, 0, sizeof(info));

            scan_ext2(dev_id, &info, 0);
            if (!info.detected) scan_fat(dev_id, &info, 0);
            if (!info.detected) scan_swap(dev_id, &info, 0);
            if (!info.detected) scan_xfs(dev_id, &info, 0);

            const char *dev_name = blockdev_name(dev_id);
            if (!dev_name) continue;

            if (info.detected) {
                found_any = 1;
                kprintf("%s: TYPE=\"%s\" UUID=\"%s\" LABEL=\"%s\"\n",
                        dev_name, info.type, info.uuid, info.label);
            }
        }

        if (!found_any) {
            /* Fall back to old-style output if no signatures found */
            kprintf("Block Device Attributes:\n");
            kprintf("========================\n\n");

            for (int dev_id = 0; dev_id < BLOCKDEV_MAX_DEVICES; dev_id++) {
                if (!blockdev_is_registered(dev_id)) continue;
                const char *dev_name = blockdev_name(dev_id);
                if (!dev_name) continue;

                uint64_t sectors = blockdev_get_sectors(dev_id);
                uint32_t mb = (uint32_t)(sectors / 2048);
                kprintf("DEV: %s\n", dev_name);
                kprintf("  TYPE: ata\n");
                kprintf("  SIZE: %u MB\n", mb);
                kprintf("  SECTORS: %llu\n", (unsigned long long)sectors);
                kprintf("  SECTOR SIZE: 512\n");
                kprintf("  BYTES: %llu\n", (unsigned long long)(sectors * 512ULL));
                kprintf("  LABEL: (none)\n");
                kprintf("  UUID: (none)\n\n");
            }
        }
    }
}
