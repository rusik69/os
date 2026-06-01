/* cmd_iostat.c — Show I/O statistics */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

/* Track previous sector counts to compute deltas */
static uint32_t prev_ata_sectors = 0;
static uint32_t prev_ahci_sectors = 0;

void cmd_iostat(const char *args) {
    (void)args;
    kprintf("I/O Statistics:\n");
    kprintf("Device              Reads(sects)   Delta\n");

    uint32_t ata_sects = libc_ata_is_present() ? libc_ata_get_sectors() : 0;
    uint32_t ahci_sects = libc_ahci_is_present() ? libc_ahci_get_sectors() : 0;

    if (libc_ata_is_present()) {
        int64_t delta = (int64_t)(ata_sects - prev_ata_sectors);
        kprintf("ata0                %-15u %lld\n", (uint64_t)ata_sects, delta);
        prev_ata_sectors = ata_sects;
    }
    if (libc_ahci_is_present()) {
        int64_t delta = (int64_t)(ahci_sects - prev_ahci_sectors);
        kprintf("ahci0               %-15u %lld\n", (uint64_t)ahci_sects, delta);
        prev_ahci_sectors = ahci_sects;
    }
    kprintf("\nNote: readings show total sector count (monotonically increasing)\n");
}
