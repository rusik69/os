/* cmd_mkswap.c — setup swap area on a block device */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "blockdev.h"

int cmd_mkswap(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: mkswap <device> [size-in-blocks]\n");
        return 1;
    }

    const char *device = argv[1];
    uint32_t blocks = 0;

    if (argc >= 3)
        blocks = (uint32_t)strtoul(argv[2], NULL, 10);

    /* Find the block device */
    int dev_id = blockdev_find_by_name(device);
    if (dev_id < 0) {
        kprintf("mkswap: block device '%s' not found\n", device);
        return 1;
    }

    uint64_t total_sectors = blockdev_get_sectors(dev_id);
    if (total_sectors == 0) {
        kprintf("mkswap: device '%s' has zero sectors\n", device);
        return 1;
    }

    if (blocks > 0 && (uint64_t)blocks > total_sectors)
        blocks = (uint32_t)total_sectors;

    kprintf("mkswap: setting up swap space on '%s'", device);
    if (blocks > 0)
        kprintf(" (%u blocks)", blocks);
    kprintf("\n");

    /*
     * Write Linux swap signature: 0x434D5350 ("SWAP" in ASCII when
     * read as a 32-bit little-endian value) at byte offset 0x1FFE
     * (the last 2 bytes of the 8192-byte area).
     *
     * 0x1FFE = 8190 decimal. That falls into sectors 15-16 (512-byte sectors).
     * Sector 15: bytes 7680-8191 (offset 0x1FFE = byte 8190 is at sector-offset 510)
     * Sector 16: bytes 8192-8703 (we need bytes 8192-8193)
     *
     * We write the signature by reading sector 15, patching bytes 510-513,
     * writing sector 15 back, then reading sector 16, patching bytes 0-1,
     * and writing sector 16 back.
     */

    /* Read sector 15 */
    uint8_t buf[512];
    int ret = blk_submit_sync(dev_id, 15, 1, buf, BLK_REQ_READ);
    if (ret < 0) {
        kprintf("mkswap: failed to read sector 15: %d\n", ret);
        return 1;
    }

    /* Write 0x434D5350 at bytes 510-513 of sector 15 */
    buf[510] = 0x50;  /* 'P' */
    buf[511] = 0x53;  /* 'S' */

    ret = blk_submit_sync(dev_id, 15, 1, buf, BLK_REQ_WRITE);
    if (ret < 0) {
        kprintf("mkswap: failed to write sector 15: %d\n", ret);
        return 1;
    }

    /* Read sector 16 */
    ret = blk_submit_sync(dev_id, 16, 1, buf, BLK_REQ_READ);
    if (ret < 0) {
        kprintf("mkswap: failed to read sector 16: %d\n", ret);
        return 1;
    }

    /* Continue the signature at bytes 0-1 of sector 16 */
    buf[0] = 0x4D;  /* 'M' */
    buf[1] = 0x43;  /* 'C' */

    ret = blk_submit_sync(dev_id, 16, 1, buf, BLK_REQ_WRITE);
    if (ret < 0) {
        kprintf("mkswap: failed to write sector 16: %d\n", ret);
        return 1;
    }

    /* Also write a swap superblock at sector 0 (first page) if blocks > 0 */
    if (blocks > 0) {
        uint32_t total_slots = (blocks * 512) / 4096;  /* 4K slots */
        if (total_slots > 1) total_slots--;  /* one slot for superblock */

        memset(buf, 0, sizeof(buf));

        /* Simple swap superblock — first few bytes */
        uint64_t magic = 0x534D415053505057ULL;  /* "SWAPSPACE" */
        memcpy(buf, &magic, 8);
        uint32_t version = 1;
        memcpy(buf + 8, &version, 4);
        memcpy(buf + 12, &total_slots, 4);

        ret = blk_submit_sync(dev_id, 0, 1, buf, BLK_REQ_WRITE);
        if (ret < 0) {
            kprintf("mkswap: failed to write superblock: %d\n", ret);
            return 1;
        }
        kprintf("mkswap: swap area with %u slots ready\n", total_slots);
    } else {
        kprintf("mkswap: swap signature written\n");
    }

    return 0;
}

void mkswap_init(void)
{
    kprintf("[OK] cmd_mkswap: swap setup command ready\n");
}
