/* cmd_ifconfig.c — ifconfig command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_ifconfig(void) {
    if (!libc_net_is_present()) { kprintf("No network device\n"); return; }
    uint8_t mac[6];
    libc_net_get_mac(mac);
    uint8_t ip[4];
    libc_net_get_ip(ip);
    uint8_t gw[4], mask[4];
    uint32_t gw32 = libc_net_get_gateway();
    uint32_t mask32 = libc_net_get_mask();
    gw[0] = (gw32 >> 24) & 0xFF; gw[1] = (gw32 >> 16) & 0xFF;
    gw[2] = (gw32 >> 8) & 0xFF;  gw[3] = gw32 & 0xFF;
    mask[0] = (mask32 >> 24) & 0xFF; mask[1] = (mask32 >> 16) & 0xFF;
    mask[2] = (mask32 >> 8) & 0xFF;  mask[3] = mask32 & 0xFF;
    kprintf("eth0:\n");
    kprintf("  MAC:  %x:%x:%x:%x:%x:%x\n",
            (unsigned long)mac[0], (unsigned long)mac[1], (unsigned long)mac[2],
            (unsigned long)mac[3], (unsigned long)mac[4], (unsigned long)mac[5]);
    kprintf("  IP:   %u.%u.%u.%u\n",
            (unsigned long)ip[0], (unsigned long)ip[1], (unsigned long)ip[2], (unsigned long)ip[3]);
    kprintf("  Mask: %u.%u.%u.%u\n",
            (unsigned long)mask[0], (unsigned long)mask[1], (unsigned long)mask[2], (unsigned long)mask[3]);
    kprintf("  GW:   %u.%u.%u.%u\n",
            (unsigned long)gw[0], (unsigned long)gw[1], (unsigned long)gw[2], (unsigned long)gw[3]);
}
