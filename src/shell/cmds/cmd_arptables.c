/* cmd_arptables.c — ARP table management */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "types.h"
#include "net.h"

/* ── Callback context for arptables list output ──────────────── */
static int arptables_entry_count;

static void arptables_list_cb(uint32_t ip, const uint8_t *mac)
{
    uint32_t h_ip = ip;
    kprintf("? (%u.%u.%u.%u) at %02x:%02x:%02x:%02x:%02x:%02x",
            (unsigned int)((h_ip >> 24) & 0xFF),
            (unsigned int)((h_ip >> 16) & 0xFF),
            (unsigned int)((h_ip >> 8) & 0xFF),
            (unsigned int)(h_ip & 0xFF),
            (unsigned int)mac[0], (unsigned int)mac[1],
            (unsigned int)mac[2], (unsigned int)mac[3],
            (unsigned int)mac[4], (unsigned int)mac[5]);
    kprintf(" [0x%04x] on eth0\n", 0x0001); /* HW type Ethernet */
    arptables_entry_count++;
}

static void arptables_usage(void)
{
    kprintf("Usage: arptables [-L] [table]\n");
    kprintf("Show ARP cache entries.\n");
}

void cmd_arptables(const char *args)
{
    if (!args || !*args) {
        /* No args — default to list */
        kprintf("ARP table:\n");
        arptables_entry_count = 0;
        net_arp_list(arptables_list_cb);
        if (arptables_entry_count == 0)
            kprintf("  (empty)\n");
        kprintf("Total entries: %d\n", arptables_entry_count);
        return;
    }

    /* Skip leading spaces */
    while (*args == ' ') args++;

    if (strncmp(args, "-L", 2) == 0) {
        kprintf("ARP table:\n");
        arptables_entry_count = 0;
        net_arp_list(arptables_list_cb);
        if (arptables_entry_count == 0)
            kprintf("  (empty)\n");
        kprintf("Total entries: %d\n", arptables_entry_count);
    } else {
        kprintf("arptables: unknown option '%s'\n", args);
        arptables_usage();
    }
}
