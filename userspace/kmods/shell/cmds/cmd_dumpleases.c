/* cmd_dumpleases.c — show DHCP leases */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "net.h"
#include "dhcp.h"

int cmd_dumpleases(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint8_t ip[4], gw[4], mask4[4];
    libc_net_get_ip(ip);
    uint32_t gw32 = libc_net_get_gateway();
    gw[0] = (uint8_t)(gw32 >> 24);
    gw[1] = (uint8_t)(gw32 >> 16);
    gw[2] = (uint8_t)(gw32 >> 8);
    gw[3] = (uint8_t)(gw32);
    uint32_t mask32 = libc_net_get_mask();
    mask4[0] = (uint8_t)(mask32 >> 24);
    mask4[1] = (uint8_t)(mask32 >> 16);
    mask4[2] = (uint8_t)(mask32 >> 8);
    mask4[3] = (uint8_t)(mask32);

    uint32_t dns = net_get_dns();
    uint8_t dns4[4];
    dns4[0] = (uint8_t)(dns >> 24);
    dns4[1] = (uint8_t)(dns >> 16);
    dns4[2] = (uint8_t)(dns >> 8);
    dns4[3] = (uint8_t)(dns);

    uint32_t server = dhcp_get_server();
    uint32_t lease_time = dhcp_get_lease_time();
    int has_lease = dhcp_has_lease_func();

    kprintf("DHCP lease information:\n");
    if (!has_lease) {
        kprintf("  No active lease\n");
        return 0;
    }

    kprintf("  IP address:     %d.%d.%d.%d\n",
            (int)ip[0], (int)ip[1], (int)ip[2], (int)ip[3]);
    kprintf("  Subnet mask:    %d.%d.%d.%d\n",
            (int)mask4[0], (int)mask4[1], (int)mask4[2], (int)mask4[3]);
    kprintf("  Gateway:        %d.%d.%d.%d\n",
            (int)gw[0], (int)gw[1], (int)gw[2], (int)gw[3]);

    if (dns != 0) {
        kprintf("  DNS server:     %d.%d.%d.%d\n",
                (int)dns4[0], (int)dns4[1], (int)dns4[2], (int)dns4[3]);
    }

    if (server != 0) {
        kprintf("  DHCP server:    %d.%d.%d.%d\n",
                (int)((server >> 24) & 0xFF),
                (int)((server >> 16) & 0xFF),
                (int)((server >> 8) & 0xFF),
                (int)(server & 0xFF));
    }

    if (lease_time > 0) {
        kprintf("  Lease time:     %u seconds\n", (unsigned int)lease_time);
    }

    return 0;
}

void dumpleases_init(void)
{
    kprintf("[OK] cmd_dumpleases: DHCP lease display ready\n");
}
