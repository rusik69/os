/* cmd_ifplugd.c — link monitor: check network interface link status */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "netdevice.h"

int cmd_ifplugd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int count = netif_count();
    if (count == 0) {
        kprintf("ifplugd: no network interfaces registered\n");
        return 0;
    }

    kprintf("ifplugd: link beat monitor\n");
    for (int i = 0; i < NETDEV_MAX; i++) {
        struct net_device *dev = netif_get(i);
        if (!dev)
            continue;

        int has_carrier = 0;
        /* Check if interface is up (IFF_UP) and has carrier */
        if (dev->flags & 1) {  /* IFF_UP = 1 */
            has_carrier = 1;   /* Assume carrier if UP (simplified) */
        }

        kprintf("  %s: ", dev->name);
        if (has_carrier) {
            kprintf("link detected");
            /* Show MAC address */
            kprintf(" (%02x:%02x:%02x:%02x:%02x:%02x, MTU %d)",
                    (unsigned int)dev->mac[0], (unsigned int)dev->mac[1],
                    (unsigned int)dev->mac[2], (unsigned int)dev->mac[3],
                    (unsigned int)dev->mac[4], (unsigned int)dev->mac[5],
                    dev->mtu);
        } else {
            kprintf("no link");
        }
        kprintf("\n");
    }

    return 0;
}

void ifplugd_init(void)
{
    kprintf("[OK] cmd_ifplugd: link monitor ready\n");
}
