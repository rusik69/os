/* cmd_arp.c — arp command */
#include "shell_cmds.h"
#include "printf.h"
#include "net.h"

static void arp_print_entry(uint32_t ip, const uint8_t *mac) {
    kprintf("  %u.%u.%u.%u  ->  %x:%x:%x:%x:%x:%x\n",
            (uint64_t)((ip >> 24) & 0xFF), (uint64_t)((ip >> 16) & 0xFF),
            (uint64_t)((ip >> 8) & 0xFF), (uint64_t)(ip & 0xFF),
            (uint64_t)mac[0], (uint64_t)mac[1], (uint64_t)mac[2],
            (uint64_t)mac[3], (uint64_t)mac[4], (uint64_t)mac[5]);
}

void cmd_arp(void) {
    kprintf("ARP cache:\n");
    int n = net_arp_list(arp_print_entry);
    if (n == 0) kprintf("  (empty)\n");
    kprintf("Entries: %u\n", (uint64_t)n);
}
