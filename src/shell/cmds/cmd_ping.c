/* cmd_ping.c — ping command */
#include "shell_cmds.h"
#include "printf.h"
#include "net.h"
#include "e1000.h"

void cmd_ping(const char *args) {
    if (!e1000_is_present()) { kprintf("No network device\n"); return; }
    uint32_t target;
    if (args && *args) {
        target = net_dns_resolve(args);
        if (!target) { kprintf("Could not resolve %s\n", args); return; }
    } else {
        target = net_get_gateway();
        if (!target) { kprintf("No gateway configured\n"); return; }
    }
    kprintf("PING %u.%u.%u.%u: ",
            (uint64_t)((target >> 24) & 0xFF), (uint64_t)((target >> 16) & 0xFF),
            (uint64_t)((target >> 8) & 0xFF), (uint64_t)(target & 0xFF));
    int ms = net_ping(target);
    if (ms < 0) {
        kprintf("Request timed out\n");
    } else {
        kprintf("Reply in %u ms\n", (uint64_t)ms);
    }
}
