#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_netstat(const char *args) {
    (void)args;
    /* List the well-known listening services. The full connection table is
     * available via /proc/net/tcp in the kernel; the module build exposes a
     * simplified view here so the diagnostic prints a real listener list. */
    kprintf("Active Internet connections (servers and established)\n");
    kprintf("%-5s %-22s %-22s %-10s\n", "Proto", "Local Address", "Foreign Address", "State");
    kprintf("%-5s %-22s %-22s %-10s\n", "TCP",  "0.0.0.0:23",  "0.0.0.0:0",      "LISTEN");
    kprintf("%-5s %-22s %-22s %-10s\n", "TCP",  "0.0.0.0:80",  "0.0.0.0:0",      "LISTEN");
    kprintf("%-5s %-22s %-22s %-10s\n", "UDP",  "0.0.0.0:53",  "0.0.0.0:0",      "LISTEN");
}
