/* cmd_ping.c — ping command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

#define MAX_ICMP_PAYLOAD 1472  /* 1500 Ethernet MTU - 20 IP hdr - 8 ICMP hdr */

void cmd_ping(const char *args) {
    if (!libc_net_is_present()) { kprintf("No network device\n"); return; }

    /* Parse args: [target] [size] */
    uint32_t target;
    int payload_size = 0;
    int size_given = 0;

    if (args && *args) {
        const char *p = args;
        while (*p == ' ') p++;

        /* First token: target host/IP */
        char host[64];
        int hi = 0;
        while (*p && *p != ' ') {
            if (hi < 63) host[hi++] = *p;
            p++;
        }
        host[hi] = '\0';

        /* Skip spaces to second token (optional size) */
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') {
            payload_size = 0;
            while (*p >= '0' && *p <= '9') {
                payload_size = payload_size * 10 + (*p - '0');
                p++;
            }
            size_given = 1;
        }

        /* Resolve target */
        target = libc_net_dns_resolve(host);
        if (!target) { kprintf("Could not resolve %s\n", host); return; }
    } else {
        target = libc_net_get_gateway();
        if (!target) { kprintf("No gateway configured\n"); return; }
    }

    /* Validate and clamp payload size */
    if (size_given) {
        if (payload_size > MAX_ICMP_PAYLOAD) {
            kprintf("Error: payload size %d exceeds maximum %d bytes\n",
                    payload_size, MAX_ICMP_PAYLOAD);
            return;
        }
        if (payload_size < 0) {
            kprintf("Error: payload size must be positive\n");
            return;
        }
    } else {
        payload_size = 32;  /* default */
    }

    /* Clamp to max (redundant with check above, but belt-and-suspenders) */
    if (payload_size > MAX_ICMP_PAYLOAD)
        payload_size = MAX_ICMP_PAYLOAD;

    kprintf("PING %u.%u.%u.%u (%d byte payload): ",
            (unsigned int)((target >> 24) & 0xFF), (unsigned int)((target >> 16) & 0xFF),
            (unsigned int)((target >> 8) & 0xFF), (unsigned int)(target & 0xFF),
            payload_size);
    int ms = libc_net_ping(target);
    if (ms < 0) {
        kprintf("Request timed out\n");
    } else {
        kprintf("Reply in %d ms\n", ms);
    }
}
