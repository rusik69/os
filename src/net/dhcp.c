#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "dhcp.h"
#include "string.h"
#include "net.h"
static uint32_t dhcp_server = 0;
static uint32_t dhcp_lease = 86400;
void dhcp_init(void) {
    kprintf("[OK] DHCP client initialized\n");
}
int dhcp_discover(void) {
    kprintf("[dhcp] Sending DISCOVER...\n");
    /* In a real kernel this would broadcast a DHCPDISCOVER */
    kprintf("[dhcp] No DHCP server found (stub)\n");
    return -1;
}
void dhcp_set_server(uint32_t ip) { dhcp_server = ip; }
uint32_t dhcp_get_server(void) { return dhcp_server; }
