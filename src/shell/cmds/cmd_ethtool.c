/* cmd_ethtool.c — Ethernet device settings */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "types.h"
#include "netdevice.h"

static void ethtool_usage(void)
{
    kprintf("Usage: ethtool <interface>\n");
    kprintf("Query Ethernet device settings.\n");
}

void cmd_ethtool(const char *args)
{
    struct net_device *dev;
    int ifindex;

    if (!args || !*args) {
        ethtool_usage();
        return;
    }

    while (*args == ' ') args++;

    /* Try to look up the interface by name */
    ifindex = netif_name_to_index(args);
    if (ifindex < 0) {
        kprintf("ethtool: unknown interface '%s'\n", args);
        kprintf("Available interfaces:\n");
        for (int i = 0; i < NETDEV_MAX; i++) {
            struct net_device *nd = netif_get(i);
            if (nd)
                kprintf("  %s (index %d)\n", nd->name, i);
        }
        return;
    }

    dev = netif_get(ifindex);
    if (!dev) {
        kprintf("ethtool: interface '%s' no longer available\n", args);
        return;
    }

    kprintf("Settings for %s:\n", dev->name);
    kprintf("  MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
            (unsigned int)dev->mac[0], (unsigned int)dev->mac[1],
            (unsigned int)dev->mac[2], (unsigned int)dev->mac[3],
            (unsigned int)dev->mac[4], (unsigned int)dev->mac[5]);
    kprintf("  MTU: %d\n", dev->mtu);

    /* Link status: if IFF_UP flag (bit 0 in flags) is set */
    if (dev->flags & 1)
        kprintf("  Link detected: yes\n");
    else
        kprintf("  Link detected: no\n");

    kprintf("  Flags: 0x%x", dev->flags);
    if (dev->flags & 1) kprintf(" UP");
    kprintf("\n");

    /* Speed / duplex — not directly available in net_device,
     * show best-effort from driver private data hint */
    kprintf("  Speed: Unknown (driver-dependent)\n");
    kprintf("  Duplex: Unknown\n");
    kprintf("  Auto-negotiation: Unknown\n");
    kprintf("  Driver info: kernel netif\n");
    kprintf("  Ifindex: %d\n", dev->ifindex);
}
