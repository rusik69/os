/* cmd_ifconfig.c — ifconfig command */
#include "shell_cmds.h"
#include "printf.h"
#include "net.h"
#include "e1000.h"

void cmd_ifconfig(void) {
    if (!e1000_is_present()) { kprintf("No network device\n"); return; }
    uint8_t mac[6];
    e1000_get_mac(mac);
    uint8_t ip[4];
    net_get_ip(ip);
    uint8_t gw[4], mask[4];
    uint32_t gw32 = net_get_gateway();
    uint32_t mask32 = net_get_mask();
    gw[0] = (gw32 >> 24) & 0xFF; gw[1] = (gw32 >> 16) & 0xFF;
    gw[2] = (gw32 >> 8) & 0xFF;  gw[3] = gw32 & 0xFF;
    mask[0] = (mask32 >> 24) & 0xFF; mask[1] = (mask32 >> 16) & 0xFF;
    mask[2] = (mask32 >> 8) & 0xFF;  mask[3] = mask32 & 0xFF;
    kprintf("eth0:\n");
    kprintf("  MAC:  %x:%x:%x:%x:%x:%x\n",
            (uint64_t)mac[0], (uint64_t)mac[1], (uint64_t)mac[2],
            (uint64_t)mac[3], (uint64_t)mac[4], (uint64_t)mac[5]);
    kprintf("  IP:   %u.%u.%u.%u\n",
            (uint64_t)ip[0], (uint64_t)ip[1], (uint64_t)ip[2], (uint64_t)ip[3]);
    kprintf("  Mask: %u.%u.%u.%u\n",
            (uint64_t)mask[0], (uint64_t)mask[1], (uint64_t)mask[2], (uint64_t)mask[3]);
    kprintf("  GW:   %u.%u.%u.%u\n",
            (uint64_t)gw[0], (uint64_t)gw[1], (uint64_t)gw[2], (uint64_t)gw[3]);
}
