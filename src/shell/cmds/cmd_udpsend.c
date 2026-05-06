/* cmd_udpsend.c — udpsend command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_udpsend(const char *args) {
    if (!args || !*args) { kprintf("Usage: udpsend <ip> <port> <data>\n"); return; }
    if (!libc_net_is_present()) { kprintf("No network device\n"); return; }

    char ipstr[20];
    int ii = 0;
    const char *p = args;
    while (*p && *p != ' ' && ii < 19) ipstr[ii++] = *p++;
    ipstr[ii] = '\0';
    while (*p == ' ') p++;

    uint32_t parts[4] = {0};
    int part = 0;
    for (int i = 0; ipstr[i] && part < 4; i++) {
        if (ipstr[i] >= '0' && ipstr[i] <= '9') parts[part] = parts[part] * 10 + (uint32_t)(ipstr[i] - '0');
        else if (ipstr[i] == '.') part++;
    }
    uint32_t dst_ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];

    uint16_t port = 0;
    while (*p >= '0' && *p <= '9') { port = (uint16_t)(port * 10 + (*p - '0')); p++; }
    while (*p == ' ') p++;

    if (!*p) { kprintf("Usage: udpsend <ip> <port> <data>\n"); return; }

    libc_net_udp_send(dst_ip, 12345, port, p, (uint16_t)strlen(p));
    kprintf("UDP sent %u bytes to %u.%u.%u.%u:%u\n",
            (uint64_t)strlen(p),
            (uint64_t)((dst_ip >> 24) & 0xFF), (uint64_t)((dst_ip >> 16) & 0xFF),
            (uint64_t)((dst_ip >> 8) & 0xFF), (uint64_t)(dst_ip & 0xFF),
            (uint64_t)port);
}
