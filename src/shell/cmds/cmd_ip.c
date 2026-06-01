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
                (uint64_t)mac[0], (uint64_t)mac[1], (uint64_t)mac[2],
                (uint64_t)mac[3], (uint64_t)mac[4], (uint64_t)mac[5]);
    } else if (strncmp(args, "route", 5) == 0) {
        uint32_t gw = libc_net_get_gateway();
        uint32_t mask = libc_net_get_mask();
        kprintf("default via %u.%u.%u.%u dev eth0\n",
                (uint64_t)((gw >> 24) & 0xFF), (uint64_t)((gw >> 16) & 0xFF),
                (uint64_t)((gw >> 8) & 0xFF), (uint64_t)(gw & 0xFF));
        kprintf("%u.%u.%u.%u/%u dev eth0 proto kernel\n",
                (uint64_t)0, (uint64_t)0, (uint64_t)0, (uint64_t)0,
                (uint64_t)(mask ? __builtin_popcount(mask) : 0));
    } else if (strncmp(args, "addr", 4) == 0) {
        uint8_t ip[4];
        libc_net_get_ip(ip);
        kprintf("1: lo: inet 127.0.0.1/8\n");
        kprintf("2: eth0: inet %u.%u.%u.%u\n",
                (uint64_t)ip[0], (uint64_t)ip[1], (uint64_t)ip[2], (uint64_t)ip[3]);
    } else {
        kprintf("ip: unknown subcommand '%s'\n", args);
    }
}
