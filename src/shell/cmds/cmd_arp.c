/* cmd_arp.c — arp command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_arp(void) {
    kprintf("ARP cache:\n");
    int n = libc_net_arp_list_print();
    if (n == 0) kprintf("  (empty)\n");
    kprintf("Entries: %u\n", (uint64_t)n);
}
