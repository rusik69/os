#include "shell_cmds.h"
#include "ata.h"
#include "ahci.h"
#include "printf.h"

void cmd_lsblk(void) {
    kprintf("NAME   SIZE    TYPE\n");
    if (ata_is_present()) {
        uint32_t sects = ata_get_sectors();
        uint32_t mb    = sects / 2048;
        kprintf("sda    %u MB   disk (ATA PIO)\n", (uint64_t)mb);
    }
    if (ahci_is_present()) {
        uint32_t sects = ahci_get_sectors();
        uint32_t mb    = sects / 2048;
        kprintf("sdb    %u MB   disk (AHCI SATA)\n", (uint64_t)mb);
    }
    if (!ata_is_present() && !ahci_is_present()) {
        kprintf("(no block devices detected)\n");
    }
}
