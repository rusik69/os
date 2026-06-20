/*
 * cmd_blkdiscard.c — Discard (TRIM) sectors on a block device
 *
 * Usage:
 *   blkdiscard <dev_id> <start_lba> <count>
 *
 * Sends a BLKDISCARD (deallocate/TRIM) command to the specified block device,
 * asking it to release the given LBA range.  Subsequent reads from the
 * deallocated range may return zeroes.
 *
 * The device ID can be found via `lsblk` or `nvme list`.
 *
 * Examples:
 *   blkdiscard 8 0 1000     — deallocate first 1000 sectors on device 8
 *   blkdiscard 8 1000 64    — deallocate 64 sectors starting at LBA 1000
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "blockdev.h"

/**
 * shell_cmd_blkdiscard — Parse args and perform block device discard.
 *
 * Parses three positional arguments (dev_id, start_lba, count) from the
 * raw argument string and calls blockdev_discard().
 */
static void shell_cmd_blkdiscard(const char *args) {
    if (!args || args[0] == '\0') {
        kprintf("Usage: blkdiscard <dev_id> <start_lba> <count>\n");
        kprintf("  dev_id:    block device ID (e.g., 8 for first NVMe namespace)\n");
        kprintf("  start_lba: first sector to deallocate\n");
        kprintf("  count:     number of sectors to deallocate\n");
        kprintf("\nExamples:\n");
        kprintf("  blkdiscard 8 0 1000\n");
        return;
    }

    /* Parse the three arguments (space-separated) */
    char buf[128];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *argv[4];
    int argc = 0;
    char *p = buf;
    while (*p && argc < 4) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
    }

    if (argc < 3) {
        kprintf("blkdiscard: too few arguments (need 3: dev_id start_lba count)\n");
        return;
    }

    int dev_id = atoi(argv[0]);
    if (dev_id < 0) {
        kprintf("blkdiscard: invalid device ID '%s'\n", argv[0]);
        return;
    }

    if (!blockdev_is_registered(dev_id)) {
        kprintf("blkdiscard: device %d is not registered\n", dev_id);
        return;
    }

    uint64_t start_lba = strtoul(argv[1], NULL, 10);
    uint32_t count = (uint32_t)strtoul(argv[2], NULL, 10);

    if (count == 0) {
        kprintf("blkdiscard: count must be > 0\n");
        return;
    }

    kprintf("[blkdiscard] Device %d (%s): deallocating %u sectors at LBA %llu...\n",
            dev_id, blockdev_name(dev_id),
            count, (unsigned long long)start_lba);

    int ret = blockdev_discard(dev_id, start_lba, count);
    if (ret < 0) {
        kprintf("[blkdiscard] FAILED: discard returned %d\n", ret);
        return;
    }

    kprintf("[blkdiscard] OK: %u sectors deallocated\n", count);
}

/* Entry point for the shell command table — expects (const char *args) */
void cmd_blkdiscard(const char *args) {
    shell_cmd_blkdiscard(args);
}

void blkdiscard_init(void)
{
    kprintf("[OK] cmd_blkdiscard: block discard command ready\n");
}
