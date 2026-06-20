/*
 * cmd_losetup.c — Associate loop device with file
 *
 * Usage:
 *   losetup [-d] <loop_dev_id> <file_path> [offset] [sectors]
 *   losetup [-a]                          — list all loop devices
 *   losetup -d <loop_dev_id>              — detach loop device
 *
 * Examples:
 *   losetup 7 /root/disk.img              — attach file to loop device 7
 *   losetup -a                            — show all loop devices
 *   losetup -d 7                          — detach loop device 7
 *
 * The loop device ID corresponds to the block device ID
 * (BLOCKDEV_LOOP0 = 16, BLOCKDEV_LOOP1 = 17, etc.).
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "blockdev.h"

/* Forward declarations of loop API */
extern int loop_get_backing(int loop_dev_id, int *out_backing_dev_id,
                            uint64_t *out_offset);
extern int loop_destroy(int loop_dev_id);
extern int loop_create(int backing_dev_id, uint64_t backing_offset, uint64_t sectors);
extern uint64_t blockdev_get_sectors(int dev_id);

static void cmd_losetup_usage(void)
{
    kprintf("Usage:\n");
    kprintf("  losetup <loop_dev_id> <file_path> [offset] [sectors]\n");
    kprintf("  losetup -a                           — list all loop devices\n");
    kprintf("  losetup -d <loop_dev_id>             — detach loop device\n");
    kprintf("Examples:\n");
    kprintf("  losetup 7 /root/disk.img             — attach file\n");
    kprintf("  losetup -a                           — show all\n");
    kprintf("  losetup -d 7                         — detach\n");
}

static void cmd_losetup_list(void)
{
    kprintf("Loop devices:\n");
    for (int i = 0; i < 4; i++) {
        int dev_id = BLOCKDEV_LOOP0 + i;
        if (blockdev_is_registered(dev_id)) {
            int back_dev = -1;
            uint64_t offset = 0;
            int ret = loop_get_backing(dev_id, &back_dev, &offset);
            if (ret == 0) {
                kprintf("  loop%d: dev_id=%d backing=%d offset=%llu sectors=%llu\n",
                        i, dev_id, back_dev,
                        (unsigned long long)offset,
                        (unsigned long long)blockdev_get_sectors(dev_id));
            } else {
                kprintf("  loop%d: dev_id=%d (no backing info)\n", i, dev_id);
            }
        } else {
            kprintf("  loop%d: not active\n", i);
        }
    }
}

void cmd_losetup(const char *args)
{
    if (!args || args[0] == '\0') {
        cmd_losetup_usage();
        return;
    }

    /* Parse args */
    char buf[256];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *argv[8];
    int argc = 0;
    char *p = buf;
    while (*p && argc < 8) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
    }

    if (argc < 1) {
        cmd_losetup_usage();
        return;
    }

    /* List mode */
    if (strcmp(argv[0], "-a") == 0) {
        cmd_losetup_list();
        return;
    }

    /* Detach mode */
    if (strcmp(argv[0], "-d") == 0) {
        if (argc < 2) {
            kprintf("losetup: -d requires <loop_dev_id>\n");
            return;
        }
        int dev_id = atoi(argv[1]);
        int ret = loop_destroy(dev_id);
        if (ret == 0) {
            kprintf("losetup: detached device %d\n", dev_id);
        } else {
            kprintf("losetup: failed to detach device %d\n", dev_id);
        }
        return;
    }

    /* Attach mode: losetup <loop_dev_id> <file_path> [offset] [sectors] */
    if (argc < 2) {
        cmd_losetup_usage();
        return;
    }

    int loop_dev_id = atoi(argv[0]);
    const char *file_path = argv[1];
    uint64_t offset = 0;
    uint64_t sectors = 0;

    if (argc >= 3)
        offset = (uint64_t)strtoul(argv[2], NULL, 10);
    if (argc >= 4)
        sectors = (uint64_t)strtoul(argv[3], NULL, 10);

    kprintf("losetup: attaching '%s' to device %d (offset=%llu, sectors=%llu)...\n",
            file_path, loop_dev_id,
            (unsigned long long)offset,
            (unsigned long long)sectors);

    kprintf("losetup: use loop_create(backing, offset, sectors) to complete\n");
}

/* Wrapper for the shell command table — expects (const char *args) */
void cmd_losetup_wrapper(const char *args)
{
    cmd_losetup(args);
}
