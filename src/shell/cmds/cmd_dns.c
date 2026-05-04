/* cmd_dns.c — dns command */
#include "shell_cmds.h"
#include "printf.h"
#include "net.h"
#include "e1000.h"

void cmd_dns(const char *args) {
    if (!args || !*args) { kprintf("Usage: dns <hostname>\n"); return; }
    if (!e1000_is_present()) { kprintf("No network device\n"); return; }
    kprintf("Resolving %s... ", args);
    uint32_t ip = net_dns_resolve(args);
    if (!ip) {
        kprintf("failed\n");
    } else {
        kprintf("%u.%u.%u.%u\n",
                (uint64_t)((ip >> 24) & 0xFF), (uint64_t)((ip >> 16) & 0xFF),
                (uint64_t)((ip >> 8) & 0xFF), (uint64_t)(ip & 0xFF));
    }
}
