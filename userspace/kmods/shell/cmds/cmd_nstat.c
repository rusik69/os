/* cmd_nstat.c — Network statistics */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "types.h"
#include "net.h"
#include "netdevice.h"

void cmd_nstat(const char *args)
{
    (void)args;

    kprintf("Network interface statistics:\n");
    kprintf("\n");

    /* Show per-interface stats using net_device info and global stats */
    int count = netif_count();
    if (count == 0) {
        kprintf("  No network interfaces registered.\n");
        return;
    }

    for (int i = 0; i < NETDEV_MAX; i++) {
        struct net_device *nd = netif_get(i);
        if (!nd) continue;

        kprintf("  %s:\n", nd->name);
        kprintf("    MAC:      %02x:%02x:%02x:%02x:%02x:%02x\n",
                (unsigned int)nd->mac[0], (unsigned int)nd->mac[1],
                (unsigned int)nd->mac[2], (unsigned int)nd->mac[3],
                (unsigned int)nd->mac[4], (unsigned int)nd->mac[5]);
        kprintf("    MTU:      %d\n", nd->mtu);
        kprintf("    Flags:    0x%x\n", nd->flags);
    }

    /* Show global interface statistics */
    kprintf("\n  Global interface counters:\n");
    kprintf("    RX packets: %lu\n",  (unsigned long)net_iface_stats.rx_packets);
    kprintf("    RX bytes:   %lu\n",  (unsigned long)net_iface_stats.rx_bytes);
    kprintf("    RX errors:  %lu\n",  (unsigned long)net_iface_stats.rx_errors);
    kprintf("    RX drops:   %lu\n",  (unsigned long)net_iface_stats.rx_drops);
    kprintf("    TX packets: %lu\n",  (unsigned long)net_iface_stats.tx_packets);
    kprintf("    TX bytes:   %lu\n",  (unsigned long)net_iface_stats.tx_bytes);
    kprintf("    TX errors:  %lu\n",  (unsigned long)net_iface_stats.tx_errors);
    kprintf("    TX drops:   %lu\n",  (unsigned long)net_iface_stats.tx_drops);
}
