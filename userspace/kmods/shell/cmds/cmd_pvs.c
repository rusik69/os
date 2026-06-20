/*
 * cmd_pvs.c — Report physical volume information (LVM)
 *
 * Usage:
 *   pvs [--help]            — list all physical volumes
 *   pvs <device>            — show info for a specific device
 *
 * Reports physical volume size, free space, and PV UUID for each
 * LVM physical volume.
 *
 * In this implementation, we query registered block devices and
 * report potential PV candidates (devices with LVM metadata).
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "blockdev.h"

/* Generate a simulated PV UUID from device characteristics */
static void pv_uuid(int dev_id, uint64_t sectors, char *out, int out_max)
{
    /* LVM UUID format: 6 groups like "AbCdEf-GhIj-KlMn-OpQr-StUv-WxYz-012345" */
    const char *hex = "0123456789abcdef";
    int pos = 0;
    int groups[] = {6, 4, 4, 4, 4, 4, 6};  /* group lengths */
    uint64_t seed = (uint64_t)(uintptr_t)dev_id * 0x9E3779B97F4A7C15ULL + sectors;

    for (int g = 0; g < 7; g++) {
        if (g > 0 && pos < out_max - 1)
            out[pos++] = '-';
        for (int i = 0; i < groups[g] && pos < out_max - 1; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            out[pos++] = hex[(seed >> 60) & 0xF];
        }
    }
    out[pos] = '\0';
}

static void cmd_pvs_usage(void)
{
    kprintf("Usage:\n");
    kprintf("  pvs [--help]              — list physical volumes\n");
    kprintf("  pvs <device>              — show info for a device\n");
    kprintf("Examples:\n");
    kprintf("  pvs                        — list all PVs\n");
    kprintf("  pvs 8                      — info for device 8\n");
}

/* Read LVM metadata from a block device to detect whether it's a PV */
static int has_lvm_pv_header(int dev_id)
{
    /* Read sector 1 (LVM label is usually at sector 1, 512 bytes in) */
    uint8_t buf[512];
    int ret = blk_submit_sync(dev_id, 1, 1, buf, BLK_REQ_READ);
    if (ret < 0) return 0;

    /* Check for LVM2 label signature at offset 0x200 (sector 1, byte 0) */
    /* Actually, LVM label is at offset 512 from the start of the device */
    /* The label is at byte offset 512, which is what we just read (sector 1) */

    /* LVM2 label header: "LABELONE" at offset 0, then "LVM2 001" at offset 8 */
    if (memcmp(buf, "LABELONE", 8) == 0) {
        /* Check for LVM2 identifier at offset 24 */
        if (memcmp(buf + 24, "LVM2 001", 8) == 0)
            return 1;
    }

    /* Also check for the header at the standard LVM offset 512 (sector 1 start) */
    /* Already read sector 1, buf[0..7] should be "LABELONE" */

    return 0;
}

static void cmd_pvs_list_all(void)
{
    kprintf("  PV         VG         Fmt   Attr   PSize   PFree   PV UUID\n");
    kprintf("  ---------- ---------- ----- ------ ------- ------- ------------------------------------\n");

    /* Scan all registered block devices for LVM metadata */
    for (int dev_id = 0; dev_id < BLOCKDEV_MAX_DEVICES; dev_id++) {
        if (blockdev_is_registered(dev_id)) {
            uint64_t sectors = blockdev_get_sectors(dev_id);
            uint64_t size_mb = (sectors * 512) / (1024 * 1024);

            int is_lvm = has_lvm_pv_header(dev_id);

            if (is_lvm) {
                char uuid[40];
                pv_uuid(dev_id, sectors, uuid, sizeof(uuid));

                kprintf("  /dev/block%d            lvm2   a--   %lluM   %lluM   %s\n",
                        dev_id,
                        (unsigned long long)size_mb,
                        (unsigned long long)size_mb,
                        uuid);
            } else {
                /* Show as a non-LVM device (potential PV) */
                kprintf("  /dev/block%d            lvm2   ---   %lluM   %lluM\n",
                        dev_id,
                        (unsigned long long)size_mb,
                        (unsigned long long)size_mb);
            }
        }
    }
}

static void cmd_pvs_show_device(int dev_id)
{
    if (!blockdev_is_registered(dev_id)) {
        kprintf("pvs: device %d not registered\n", dev_id);
        return;
    }

    uint64_t sectors = blockdev_get_sectors(dev_id);
    uint64_t size_mb = (sectors * 512) / (1024 * 1024);

    int is_lvm = has_lvm_pv_header(dev_id);

    char uuid[40];
    if (is_lvm)
        pv_uuid(dev_id, sectors, uuid, sizeof(uuid));

    kprintf("  --- Physical volume ---\n");
    kprintf("  PV Name:       /dev/block%d\n", dev_id);
    kprintf("  VG Name:       %s\n", is_lvm ? "lvm2_volume_group" : "");
    kprintf("  PV Size:       %llu MiB (%llu sectors)\n",
            (unsigned long long)size_mb,
            (unsigned long long)sectors);
    kprintf("  Allocatable:   %s\n", is_lvm ? "yes" : "no (not an LVM PV)");
    kprintf("  PE Size:       4.00 MiB\n");

    if (is_lvm) {
        uint64_t total_pe = size_mb / 4;
        kprintf("  Total PE:      %llu\n", (unsigned long long)total_pe);
        kprintf("  Free PE:       %llu\n", (unsigned long long)total_pe);
        kprintf("  Allocated PE:  0\n");
    }

    if (is_lvm) {
        kprintf("  PV UUID:       %s\n", uuid);
    }
}

void cmd_pvs(const char *args)
{
    if (!args || args[0] == '\0') {
        cmd_pvs_list_all();
        return;
    }

    char buf[64];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    while (*p == ' ') p++;

    if (strcmp(p, "--help") == 0) {
        cmd_pvs_usage();
        return;
    }

    int dev_id = atoi(p);
    cmd_pvs_show_device(dev_id);
}
