/* cmd_lshw.c — Show hardware configuration */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

void cmd_lshw(const char *args) {
    (void)args;
    kprintf("Hardware Configuration:\n");
    kprintf("=======================\n\n");

    /* CPU info */
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)&vendor[0] = ebx;
    *(uint32_t*)&vendor[4] = edx;
    *(uint32_t*)&vendor[8] = ecx;
    vendor[12] = '\0';

    char brand[49];
    memset(brand, 0, 49);
    uint32_t max_ext;
    __asm__ volatile("cpuid" : "=a"(max_ext) : "a"(0x80000000) : "ebx","ecx","edx");
    if (max_ext >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000002 + i));
            *(uint32_t*)&brand[i*16+0] = eax;
            *(uint32_t*)&brand[i*16+4] = ebx;
            *(uint32_t*)&brand[i*16+8] = ecx;
            *(uint32_t*)&brand[i*16+12] = edx;
        }
    }

    kprintf("*-cpu\n");
    kprintf("   description: CPU\n");
    kprintf("   vendor: %s\n", vendor);
    if (brand[0]) kprintf("   product: %s\n", brand);
    kprintf("   physical id: 0\n");

    /* Memory */
    struct pmm_stats stats = {0};
    pmm_get_stats(&stats);
    uint32_t total_mb = (stats.total_pages * 4) / 1024;
    kprintf("*-memory\n");
    kprintf("   description: System Memory\n");
    kprintf("   size: %uMB\n", total_mb);

    /* PCI devices */
    kprintf("*-pci\n");
    kprintf("   description: PCI bus\n");
    libc_pci_list();

    /* ATA / AHCI */
    kprintf("*-storage\n");
    if (libc_ata_is_present()) {
        uint32_t ata_sects = libc_ata_get_sectors();
        uint32_t ata_mb = ata_sects / 2048;
        kprintf("   description: ATA Disk\n");
        kprintf("   size: %uMB (%u sectors)\n", ata_mb, ata_sects);
    }
    if (libc_ahci_is_present()) {
        uint32_t ahci_sects = libc_ahci_get_sectors();
        uint32_t ahci_mb = ahci_sects / 2048;
        kprintf("   description: AHCI Disk\n");
        kprintf("   size: %uMB (%u sectors)\n", ahci_mb, ahci_sects);
    }

    /* Network */
    if (libc_net_is_present()) {
        uint8_t mac[6], ip[4];
        libc_net_get_mac(mac);
        libc_net_get_ip(ip);
        kprintf("*-network\n");
        kprintf("   description: Ethernet interface\n");
        kprintf("   logical name: eth0\n");
        kprintf("   mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
                (unsigned int)mac[0], (unsigned int)mac[1], (unsigned int)mac[2],
                (unsigned int)mac[3], (unsigned int)mac[4], (unsigned int)mac[5]);
        kprintf("   ip: %u.%u.%u.%u\n",
                (unsigned int)ip[0], (unsigned int)ip[1], (unsigned int)ip[2], (unsigned int)ip[3]);
    }
}
