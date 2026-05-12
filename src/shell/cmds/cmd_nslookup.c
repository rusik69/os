/* cmd_nslookup.c — DNS lookup utility */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_nslookup(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: nslookup <hostname>\n");
        return;
    }

    char host[128];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 127) { host[i] = args[i]; i++; }
    host[i] = '\0';

    if (!libc_net_is_present()) {
        kprintf("nslookup: no network adapter\n");
        return;
    }

    uint32_t ip = libc_net_dns_resolve(host);
    if (ip == 0) {
        kprintf("nslookup: could not resolve '%s'\n", host);
        return;
    }

    kprintf("Server: (default DNS)\n");
    kprintf("Name:   %s\n", host);
    kprintf("Address: %u.%u.%u.%u\n",
        (uint64_t)((ip >> 24) & 0xFF),
        (uint64_t)((ip >> 16) & 0xFF),
        (uint64_t)((ip >>  8) & 0xFF),
        (uint64_t)( ip        & 0xFF));
}
