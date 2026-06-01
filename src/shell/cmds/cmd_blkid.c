/* cmd_blkid.c — Show block device attributes */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"
#include "ata.h"

void cmd_blkid(const char *args) {
    (void)args;
    kprintf("Block Device Attributes:\n");
    kprintf("========================\n\n");

    /* ATA disk */
    if (ata_is_present()) {
        uint32_t sectors = ata_get_sectors();
        uint32_t mb = sectors / 2048;
        uint64_t bytes = (uint64_t)sectors * 512ULL;
        kprintf("DEV: ata0\n");
        kprintf("  TYPE: ata\n");
        kprintf("  SIZE: %u MB\n", (unsigned long)mb);
        kprintf("  SECTORS: %u\n", (unsigned long)sectors);
        kprintf("  SECTOR SIZE: 512\n");
        kprintf("  BYTES: %llu\n", bytes);
        kprintf("  LABEL: (none)\n");
        kprintf("  UUID: (none)\n\n");
    }

    /* AHCI disk */
    if (ahci_is_present()) {
        uint32_t sectors = ahci_get_sectors();
        uint32_t mb = sectors / 2048;
        uint64_t bytes = (uint64_t)sectors * 512ULL;
        kprintf("DEV: ahci0\n");
        kprintf("  TYPE: ahci\n");
        kprintf("  SIZE: %u MB\n", (unsigned long)mb);
        kprintf("  SECTORS: %u\n", (unsigned long)sectors);
        kprintf("  SECTOR SIZE: 512\n");
        kprintf("  BYTES: %llu\n", bytes);
        kprintf("  LABEL: (none)\n");
        kprintf("  UUID: (none)\n\n");
    }

    /* Ramdisk */
    kprintf("DEV: ram0\n");
    kprintf("  TYPE: ramdisk\n");
    kprintf("  SIZE: 2 MB\n");
    kprintf("  SECTORS: 4096\n");
    kprintf("  SECTOR SIZE: 512\n");
    kprintf("  LABEL: ramdisk\n");
    kprintf("  UUID: (none)\n");
}
