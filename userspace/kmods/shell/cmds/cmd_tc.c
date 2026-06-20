/* cmd_tc.c — Traffic control */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "types.h"
#include "net.h"
#include "netdevice.h"

static void tc_usage(void)
{
    kprintf("Usage: tc qdisc show dev <interface>\n");
    kprintf("       tc -s qdisc show dev <interface>\n");
    kprintf("Show queuing disciplines and interface statistics.\n");
}

void cmd_tc(const char *args)
{
    int show_stats = 0;
    const char *ifname = NULL;

    if (!args || !*args) {
        tc_usage();
        return;
    }

    while (*args == ' ') args++;

    /* Parse: tc [-s] qdisc show dev <ifname> */
    if (strncmp(args, "-s", 2) == 0) {
        show_stats = 1;
        args += 2;
        while (*args == ' ') args++;
    }

    if (strncmp(args, "qdisc", 5) != 0) {
        tc_usage();
        return;
    }
    args += 5;
    while (*args == ' ') args++;

    if (strncmp(args, "show", 4) != 0) {
        tc_usage();
        return;
    }
    args += 4;
    while (*args == ' ') args++;

    if (strncmp(args, "dev", 3) == 0) {
        args += 3;
        while (*args == ' ') args++;
        if (*args) {
            ifname = args;
            /* Strip trailing spaces */
        }
    }

    if (!ifname) {
        /* Show all interfaces */
        kprintf("qdisc pfifo_fast 0: dev lo root refcnt 2\n");
        for (int i = 0; i < NETDEV_MAX; i++) {
            struct net_device *nd = netif_get(i);
            if (!nd) continue;
            kprintf("qdisc pfifo_fast 0: dev %s root refcnt 2\n", nd->name);
            if (show_stats) {
                kprintf(" Sent %lu bytes %lu pkt\n",
                        (unsigned long)net_iface_stats.tx_bytes,
                        (unsigned long)net_iface_stats.tx_packets);
                kprintf(" backlog 0b %dp requeues 0\n");
            }
        }
        return;
    }

    /* Show specific interface */
    int ifindex = netif_name_to_index(ifname);
    if (ifindex < 0) {
        kprintf("tc: unknown interface '%s'\n", ifname);
        return;
    }

    struct net_device *nd = netif_get(ifindex);
    if (!nd) {
        kprintf("tc: interface '%s' no longer available\n", ifname);
        return;
    }

    kprintf("qdisc pfifo_fast 0: dev %s root refcnt 2\n", nd->name);
    if (show_stats) {
        kprintf(" Sent %lu bytes %lu pkt (dropped %lu, overlimits 0)\n",
                (unsigned long)(net_iface_stats.tx_bytes),
                (unsigned long)(net_iface_stats.tx_packets),
                (unsigned long)(net_iface_stats.tx_drops));
        kprintf(" backlog 0b %dp requeues 0\n");
    }
}
