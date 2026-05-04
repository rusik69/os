/* cmd_route.c — route command */
#include "shell_cmds.h"
#include "printf.h"
#include "net.h"

void cmd_route(void) {
    uint32_t gw = net_get_gateway();
    uint32_t mask = net_get_mask();
    uint8_t ip[4];
    net_get_ip(ip);
    kprintf("Routing table:\n");
    kprintf("Destination     Gateway         Mask            Iface\n");
    kprintf("%-15s %-15s %u.%u.%u.%u   eth0\n",
            "0.0.0.0", "0.0.0.0",
            (uint64_t)((mask >> 24) & 0xFF), (uint64_t)((mask >> 16) & 0xFF),
            (uint64_t)((mask >> 8) & 0xFF), (uint64_t)(mask & 0xFF));
    kprintf("%-15s %u.%u.%u.%u  %u.%u.%u.%u   eth0\n",
            "default",
            (uint64_t)((gw >> 24) & 0xFF), (uint64_t)((gw >> 16) & 0xFF),
            (uint64_t)((gw >> 8) & 0xFF), (uint64_t)(gw & 0xFF),
            (uint64_t)((mask >> 24) & 0xFF), (uint64_t)((mask >> 16) & 0xFF),
            (uint64_t)((mask >> 8) & 0xFF), (uint64_t)(mask & 0xFF));
}
