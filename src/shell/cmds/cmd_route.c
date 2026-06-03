/* cmd_route.c — route command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_route(void) {
    uint32_t gw = libc_net_get_gateway();
    uint32_t mask = libc_net_get_mask();
    uint8_t ip[4];
    libc_net_get_ip(ip);
    kprintf("Routing table:\n");
    kprintf("Destination     Gateway         Mask            Iface\n");
    kprintf("%-15s %-15s %u.%u.%u.%u   eth0\n",
            "0.0.0.0", "0.0.0.0",
            (unsigned int)((mask >> 24) & 0xFF), (unsigned int)((mask >> 16) & 0xFF),
            (unsigned int)((mask >> 8) & 0xFF), (unsigned int)(mask & 0xFF));
    kprintf("%-15s %u.%u.%u.%u  %u.%u.%u.%u   eth0\n",
            "0.0.0.0",
            (unsigned int)((gw >> 24) & 0xFF), (unsigned int)((gw >> 16) & 0xFF),
            (unsigned int)((gw >> 8) & 0xFF), (unsigned int)(gw & 0xFF),
            (unsigned int)((mask >> 24) & 0xFF), (unsigned int)((mask >> 16) & 0xFF),
            (unsigned int)((mask >> 8) & 0xFF), (unsigned int)(mask & 0xFF));
}
