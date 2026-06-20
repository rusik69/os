/* cmd_mkdosfs.c — Create FAT12/FAT16/FAT32 filesystem (S152)
 *
 * Usage:
 *   mkdosfs [-F FAT-size] [-n label] [-i volid] <device> [block-count]
 *
 * Options:
 *   -F FAT-size   FAT type: 12, 16, or 32 (auto-detected from size if omitted)
 *   -n label      Volume label (11 chars max, uppercase)
 *   -i volid      Volume ID (32-bit hex value)
 *
 * If block-count is omitted, uses the full device size.
 *
 * Writes a proper boot sector with BPB (BIOS Parameter Block).
 */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "blockdev.h"

/* ── FAT BPB on-disk structures ─────────────────────────────────────── */

/* BPB common to all FAT types */
struct fat_bpb_common {
    uint8_t  jump[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;         /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} __attribute__((packed));

/* FAT32 extended BPB (appended after common part) */
struct fat32_ext_bpb {
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed));

/* Combined FAT32 boot sector (512 bytes total) */
struct fat32_boot_sector {
    struct fat_bpb_common common;
    struct fat32_ext_bpb  ext;
    uint8_t               boot_code[420];  /* rest of 512 bytes */
    uint16_t              boot_signature;  /* 0xAA55 */
} __attribute__((packed));

/* FAT12/16 extended BPB */
struct fat16_ext_bpb {
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
    uint8_t  boot_code[448];
    uint16_t boot_signature;  /* 0xAA55 */
} __attribute__((packed));

/* ── Constants ──────────────────────────────────────────────────────── */

#define SECTOR_SIZE 512
#define BOOT_SIGNATURE 0xAA55
#define FAT12_MAX_CLUSTERS 4085    /* < 4085 = FAT12 */
#define FAT16_MAX_CLUSTERS 65525   /* < 65525 = FAT16, >= = FAT32 */

/* Media descriptor byte */
#define MEDIA_FIXED 0xF8

/* ── Defaults ───────────────────────────────────────────────────────── */

#define DEFAULT_OEM_NAME "HERMESOS "
#define DEFAULT_VOLUME_LABEL "NO NAME    "
#define DEFAULT_FS_TYPE_FAT32 "FAT32   "
#define DEFAULT_FS_TYPE_FAT16 "FAT16   "
#define DEFAULT_FS_TYPE_FAT12 "FAT12   "

/* ── Helper: pad to 11 chars, uppercase ────────────────────────────── */
static void format_label(const char *in, char *out)
{
    int i;
    for (i = 0; i < 11; i++) {
        if (in && in[i] && in[i] >= 0x20) {
            char c = in[i];
            if (c >= 'a' && c <= 'z')
                c -= 32;  /* uppercase */
            out[i] = c;
        } else {
            out[i] = ' ';
        }
    }
}

/* ── Core implementation (argc/argv based) ──────────────────────────── */

static int mkdosfs_core(int argc, char **argv)
{
    const char *device = NULL;
    const char *vol_label = DEFAULT_VOLUME_LABEL;
    uint32_t vol_id = 0;
    int fat_type = 0;        /* 0 = auto, 12/16/32 = force */
    uint32_t block_count = 0;
    int dev_id = -1;
    int use_vol_id = 0;

    if (argc < 2) {
        kprintf("usage: mkdosfs [-F 12|16|32] [-n label] [-i volid] <device> [block-count]\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) {
            fat_type = atoi(argv[++i]);
            if (fat_type != 12 && fat_type != 16 && fat_type != 32) {
                kprintf("mkdosfs: invalid FAT type %d (must be 12, 16, or 32)\n", fat_type);
                return 1;
            }
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            vol_label = argv[++i];
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            vol_id = (uint32_t)strtol(argv[++i], NULL, 16);
            use_vol_id = 1;
        } else if (argv[i][0] != '-') {
            device = argv[i];
            if (i + 1 < argc && argv[i+1][0] != '-') {
                block_count = (uint32_t)atoi(argv[++i]);
            }
        }
    }

    if (!device) {
        kprintf("mkdosfs: no device specified\n");
        return 1;
    }

    /* Open device */
    dev_id = blockdev_find_by_name(device);
    if (dev_id < 0) {
        dev_id = atoi(device);
    }

    if (dev_id < 0 || !blockdev_is_registered(dev_id)) {
        kprintf("mkdosfs: cannot open '%s'\n", device);
        return 1;
    }

    /* Get device size */
    uint64_t total_sectors = blockdev_get_sectors(dev_id);
    if (block_count == 0)
        block_count = (uint32_t)total_sectors;

    /* ── Auto-detect FAT type ─────────────────────────────────────── */
    uint32_t data_sectors;
    uint32_t bytes_per_sector = SECTOR_SIZE;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t root_dir_sectors;
    uint32_t num_fats = 2;
    uint32_t fat_size;
    uint32_t total_clusters;

    if (fat_type == 0) {
        if (block_count <= 8400) {
            fat_type = 12;
        } else if (block_count <= 65536 * 32) {
            fat_type = 16;
        } else {
            fat_type = 32;
        }
    }

    /* ── Calculate geometry ───────────────────────────────────────── */
    if (fat_type == 32) {
        if (block_count <= 532480)
            sectors_per_cluster = 1;
        else if (block_count <= 16777216)
            sectors_per_cluster = 8;
        else
            sectors_per_cluster = 32;
    } else if (fat_type == 16) {
        if (block_count <= 8400)
            sectors_per_cluster = 2;
        else if (block_count <= 42000)
            sectors_per_cluster = 4;
        else
            sectors_per_cluster = 8;
    } else {
        sectors_per_cluster = 1;
    }

    reserved_sectors = (fat_type == 32) ? 32 : 1;

    uint32_t root_entries = (fat_type == 32) ? 0 : 512;
    root_dir_sectors = (root_entries * 32 + bytes_per_sector - 1) / bytes_per_sector;

    /* Calculate FAT size iteratively */
    for (int iter = 0; iter < 3; iter++) {
        data_sectors = block_count - reserved_sectors - root_dir_sectors;
        if (fat_type == 32)
            data_sectors -= reserved_sectors;
        total_clusters = data_sectors / sectors_per_cluster;

        if (fat_type == 32) {
            fat_size = ((total_clusters * 4) + bytes_per_sector - 1) / bytes_per_sector;
        } else if (fat_type == 16) {
            fat_size = ((total_clusters * 2) + bytes_per_sector - 1) / bytes_per_sector;
        } else {
            fat_size = ((total_clusters * 3 + 1) / 2 + bytes_per_sector - 1) / bytes_per_sector;
        }

        data_sectors = block_count - reserved_sectors - num_fats * fat_size - root_dir_sectors;
        total_clusters = data_sectors / sectors_per_cluster;

        if (fat_type == 12 && total_clusters >= FAT12_MAX_CLUSTERS) {
            sectors_per_cluster++;
            continue;
        }
        if (fat_type == 16 && total_clusters >= FAT16_MAX_CLUSTERS) {
            sectors_per_cluster++;
            continue;
        }
        break;
    }

    if (total_clusters < 2) {
        kprintf("mkdosfs: device too small (%u sectors)\n", (unsigned int)block_count);
        return 1;
    }

    kprintf("mkdosfs: creating FAT%d filesystem on '%s'\n", fat_type, device);
    kprintf("  Total sectors: %u\n", (unsigned int)block_count);
    kprintf("  Sectors per cluster: %u\n", (unsigned int)sectors_per_cluster);
    kprintf("  Reserved sectors: %u\n", (unsigned int)reserved_sectors);
    kprintf("  FAT size: %u sectors\n", (unsigned int)fat_size);
    kprintf("  Number of FATs: %u\n", (unsigned int)num_fats);
    kprintf("  Root dir sectors: %u\n", (unsigned int)root_dir_sectors);
    kprintf("  Data sectors: %u\n", (unsigned int)data_sectors);
    kprintf("  Total clusters: %u\n", (unsigned int)total_clusters);

    if (!use_vol_id) {
        vol_id = 0;
        const char *p = device;
        while (*p) vol_id = (vol_id << 5) - vol_id + (uint8_t)(*p++);
        vol_id ^= block_count;
    }

    /* ── Allocate sector buffer ───────────────────────────────────── */
    uint8_t *sector = (uint8_t *)malloc(SECTOR_SIZE);
    if (!sector) {
        kprintf("mkdosfs: out of memory\n");
        return 1;
    }
    memset(sector, 0, SECTOR_SIZE);

    /* ── Write boot sector ────────────────────────────────────────── */
    if (fat_type == 32) {
        struct fat32_boot_sector *bs = (struct fat32_boot_sector *)sector;

        bs->common.jump[0] = 0xEB;
        bs->common.jump[1] = 0x3C;
        bs->common.jump[2] = 0x90;
        memcpy(bs->common.oem_name, DEFAULT_OEM_NAME, 8);
        bs->common.bytes_per_sector = bytes_per_sector;
        bs->common.sectors_per_cluster = (uint8_t)sectors_per_cluster;
        bs->common.reserved_sectors = (uint16_t)reserved_sectors;
        bs->common.num_fats = (uint8_t)num_fats;
        bs->common.root_entry_count = 0;
        bs->common.total_sectors_16 = 0;
        bs->common.media_type = MEDIA_FIXED;
        bs->common.fat_size_16 = 0;
        bs->common.sectors_per_track = 63;
        bs->common.num_heads = 16;
        bs->common.hidden_sectors = 0;
        bs->common.total_sectors_32 = block_count;

        bs->ext.fat_size_32 = fat_size;
        bs->ext.ext_flags = 0;
        bs->ext.fs_version = 0;
        bs->ext.root_cluster = 2;
        bs->ext.fs_info = 1;
        bs->ext.backup_boot_sector = 6;
        memset(bs->ext.reserved, 0, 12);
        bs->ext.drive_number = 0x80;
        bs->ext.reserved1 = 0;
        bs->ext.boot_signature = 0x29;
        bs->ext.volume_id = vol_id;
        format_label(vol_label, bs->ext.volume_label);
        memcpy(bs->ext.fs_type, DEFAULT_FS_TYPE_FAT32, 8);

        bs->boot_signature = BOOT_SIGNATURE;
    } else {
        struct fat_bpb_common *bpb = (struct fat_bpb_common *)sector;
        struct fat16_ext_bpb *ext = (struct fat16_ext_bpb *)(sector + sizeof(struct fat_bpb_common));

        bpb->jump[0] = 0xEB;
        bpb->jump[1] = 0x3C;
        bpb->jump[2] = 0x90;
        memcpy(bpb->oem_name, DEFAULT_OEM_NAME, 8);
        bpb->bytes_per_sector = bytes_per_sector;
        bpb->sectors_per_cluster = (uint8_t)sectors_per_cluster;
        bpb->reserved_sectors = (uint16_t)reserved_sectors;
        bpb->num_fats = (uint8_t)num_fats;
        bpb->root_entry_count = (uint16_t)root_entries;
        bpb->total_sectors_16 = (block_count < 65536) ? (uint16_t)block_count : 0;
        bpb->media_type = MEDIA_FIXED;
        bpb->fat_size_16 = (uint16_t)fat_size;
        bpb->sectors_per_track = 63;
        bpb->num_heads = 16;
        bpb->hidden_sectors = 0;
        bpb->total_sectors_32 = (block_count >= 65536) ? block_count : 0;

        ext->drive_number = 0x80;
        ext->reserved1 = 0;
        ext->boot_signature = 0x29;
        ext->volume_id = vol_id;
        format_label(vol_label, ext->volume_label);
        if (fat_type == 12)
            memcpy(ext->fs_type, DEFAULT_FS_TYPE_FAT12, 8);
        else
            memcpy(ext->fs_type, DEFAULT_FS_TYPE_FAT16, 8);

        ext->boot_signature = BOOT_SIGNATURE;
    }

    /* Write boot sector to device */
    if (blockdev_write_sectors(dev_id, 0, 1, sector) < 0) {
        kprintf("mkdosfs: failed to write boot sector\n");
        free(sector);
        return 1;
    }
    kprintf("  Boot sector written\n");

    /* ── Write FSINFO sector (FAT32 only) ─────────────────────────── */
    if (fat_type == 32) {
        memset(sector, 0, SECTOR_SIZE);
        sector[0] = 0x52; sector[1] = 0x52; sector[2] = 0x61; sector[3] = 0x41;
        sector[484] = 0x72; sector[485] = 0x72; sector[486] = 0x41; sector[487] = 0x61;
        *(uint32_t *)(sector + 488) = total_clusters - 1;
        *(uint32_t *)(sector + 492) = 2;
        sector[508] = 0x00; sector[509] = 0x00; sector[510] = 0x55; sector[511] = 0xAA;

        if (blockdev_write_sectors(dev_id, 1, 1, sector) < 0)
            kprintf("mkdosfs: warning: failed to write FSINFO sector\n");
        else
            kprintf("  FSINFO sector written\n");

        if (blockdev_write_sectors(dev_id, 6, 1, sector) < 0)
            kprintf("mkdosfs: warning: failed to write backup boot sector\n");
    }

    /* ── Write FATs ───────────────────────────────────────────────── */
    memset(sector, 0, SECTOR_SIZE);
    uint32_t fat_start = reserved_sectors;

    if (fat_type == 32) {
        sector[0] = (uint8_t)(MEDIA_FIXED | 0x0F);
        sector[1] = 0xFF;
        sector[2] = 0xFF;
        sector[3] = 0x0F;
        sector[4] = 0xFF;
        sector[5] = 0xFF;
        sector[6] = 0xFF;
        sector[7] = 0x0F;
    } else if (fat_type == 16) {
        sector[0] = MEDIA_FIXED;
        sector[1] = 0xFF;
        sector[2] = 0xFF;
        sector[3] = 0xFF;
    } else {
        sector[0] = MEDIA_FIXED;
        sector[1] = 0xFF;
        sector[2] = 0xFF;
        sector[3] = 0xFF;
    }

    for (uint32_t fat_copy = 0; fat_copy < num_fats; fat_copy++) {
        uint32_t fat_lba = fat_start + fat_copy * fat_size;
        for (uint32_t s = 0; s < fat_size; s++) {
            if (blockdev_write_sectors(dev_id, fat_lba + s, 1, sector) < 0) {
                kprintf("mkdosfs: failed to write FAT copy %u sector %u\n",
                        (unsigned int)fat_copy, (unsigned int)s);
                free(sector);
                return 1;
            }
            if (s == 0)
                memset(sector, 0, SECTOR_SIZE);
        }
    }
    kprintf("  FAT table(s) written (%u copies x %u sectors)\n",
            (unsigned int)num_fats, (unsigned int)fat_size);

    /* ── Write root directory ─────────────────────────────────────── */
    memset(sector, 0, SECTOR_SIZE);
    uint32_t root_dir_lba = fat_start + num_fats * fat_size;

    struct {
        char     name[11];
        uint8_t  attr;
        uint8_t  reserved[10];
        uint16_t time;
        uint16_t date;
        uint16_t cluster;
        uint32_t size;
    } __attribute__((packed)) *vol_entry = (void *)sector;

    format_label(vol_label, vol_entry->name);
    vol_entry->attr = 0x08;

    if (fat_type == 32) {
        uint32_t root_dir_cluster_lba = reserved_sectors + num_fats * fat_size;
        if (blockdev_write_sectors(dev_id, root_dir_cluster_lba, 1, sector) < 0) {
            kprintf("mkdosfs: failed to write root directory\n");
            free(sector);
            return 1;
        }
    } else {
        for (uint32_t s = 0; s < root_dir_sectors; s++) {
            if (blockdev_write_sectors(dev_id, root_dir_lba + s, 1, sector) < 0) {
                kprintf("mkdosfs: failed to write root directory sector %u\n",
                        (unsigned int)s);
                free(sector);
                return 1;
            }
            if (s == 0)
                memset(sector, 0, SECTOR_SIZE);
        }
    }
    kprintf("  Root directory written\n");

    free(sector);
    kprintf("mkdosfs: Done. FAT%d filesystem created on '%s'\n", fat_type, device);
    return 0;
}

/* ── Shell-compatible wrapper ─────────────────────────────────────────
 * The shell dispatches with `(const char *args)`. We split into argc/argv
 * and call mkdosfs_core.
 */

#define MKFS_MAX_ARGS 16

void cmd_mkdosfs(const char *args)
{
    char *argv[MKFS_MAX_ARGS];
    int argc = 0;
    char buf[256];

    if (!args || !args[0]) {
        const char *dummy_argv[] = { "mkdosfs", NULL };
        mkdosfs_core(1, (char **)dummy_argv);
        return;
    }

    size_t alen = strlen(args);
    if (alen >= sizeof(buf)) alen = sizeof(buf) - 1;
    memcpy(buf, args, alen);
    buf[alen] = '\0';

    argv[argc++] = "mkdosfs";

    char *token = strtok(buf, " ");
    while (token && argc < MKFS_MAX_ARGS - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;

    mkdosfs_core(argc, argv);
}

void mkdosfs_init(void)
{
    kprintf("[OK] cmd_mkdosfs: FAT filesystem creator ready (FAT12/16/32)\n");
}
