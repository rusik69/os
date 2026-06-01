/* cmd_ip.c — Show/manipulate routing, devices, policy routing */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_ip(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: ip <subcommand> [options]\n");
        kprintf("Subcommands:\n");
        kprintf("  link     - Show network interfaces\n");
        kprintf("  route    - Show routing table\n");
        kprintf("  addr     - Show IP addresses\n");
        return;
    }
    /* Skip spaces */
    while (*args == ' ') args++;
    if (strncmp(args, "link", 4) == 0) {
        uint8_t mac[6];
        libc_net_get_mac(mac);
        kprintf("1: lo: <LOOPBACK> mtu 65536\n");
        kprintf("    link/loopback\n");
        kprintf("2: eth0: <BROADCAST,MULTICAST,UP> mtu 1500\n");
        kprintf("    link/ether %02x:%02x:%02x:%02x:%02x:%02x\n",
                (unsigned long)mac[0], (unsigned long)mac[1], (unsigned long)mac[2],
                (unsigned long)mac[3], (unsigned long)mac[4], (unsigned long)mac[5]);
    } else if (strncmp(args, "route", 5) == 0) {
        uint32_t gw = libc_net_get_gateway();
        uint32_t mask = libc_net_get_mask();
        kprintf("default via %u.%u.%u.%u dev eth0\n",
                (unsigned long)((gw >> 24) & 0xFF), (unsigned long)((gw >> 16) & 0xFF),
                (unsigned long)((gw >> 8) & 0xFF), (unsigned long)(gw & 0xFF));
        kprintf("%u.%u.%u.%u/%u dev eth0 proto kernel\n",
                (unsigned long)0, (unsigned long)0, (unsigned long)0, (unsigned long)0,
                (unsigned long)(mask ? __builtin_popcount(mask) : 0));
    } else if (strncmp(args, "addr", 4) == 0) {
        uint8_t ip[4];
        libc_net_get_ip(ip);
        kprintf("1: lo: inet 127.0.0.1/8\n");
        kprintf("2: eth0: inet %u.%u.%u.%u\n",
                (unsigned long)ip[0], (unsigned long)ip[1], (unsigned long)ip[2], (unsigned long)ip[3]);
    } else {
        kprintf("ip: unknown subcommand '%s'\n", args);
    }
}
