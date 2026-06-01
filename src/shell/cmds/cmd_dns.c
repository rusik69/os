/* cmd_dns.c — dns command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_dns(const char *args) {
    if (!args || !*args) { kprintf("Usage: dns <hostname>\n"); return; }
    if (!libc_net_is_present()) { kprintf("No network device\n"); return; }
    kprintf("Resolving %s... ", args);
    uint32_t ip = libc_net_dns_resolve(args);
    if (!ip) {
        kprintf("failed\n");
    } else {
        kprintf("%u.%u.%u.%u\n",
                (unsigned long)((ip >> 24) & 0xFF), (unsigned long)((ip >> 16) & 0xFF),
                (unsigned long)((ip >> 8) & 0xFF), (unsigned long)(ip & 0xFF));
    }
}
