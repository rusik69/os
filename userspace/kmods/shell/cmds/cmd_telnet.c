/* cmd_telnet.c — Telnet client (connect to host:port) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_telnet(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: telnet <host> <port>\n");
        return;
    }
    char host[64]; int port = 0;
    int i = 0;
    while (args[i] == ' ') i++;
    int j = 0;
    while (args[i] && args[i] != ' ' && j < 63) host[j++] = args[i++];
    host[j] = '\0';
    while (args[i] == ' ') i++;
    while (args[i] >= '0' && args[i] <= '9') { port = port * 10 + (args[i] - '0'); i++; }

    if (!host[0] || port <= 0) {
        kprintf("Usage: telnet <host> <port>\n");
        return;
    }

    uint32_t ip = libc_net_dns_resolve(host);
    if (!ip) {
        kprintf("telnet: cannot resolve '%s'\n", host);
        return;
    }

    int conn = libc_net_tcp_connect(ip, (uint16_t)port);
    if (conn < 0) {
        kprintf("telnet: connection to %s:%d failed\n", host, port);
        return;
    }
    kprintf("Connected to %s:%d\n", host, port);
    /* Send a simple HTTP-like request to test */
    char req[256];
    snprintf(req, sizeof(req), "GET / HTTP/1.0\r\nHost: %s\r\n\r\n", host);
    libc_net_tcp_send(conn, req, (uint16_t)strlen(req));

    static char buf[1024];
    int n = libc_net_tcp_recv(conn, buf, sizeof(buf) - 1, 100);
    if (n > 0) { buf[n] = '\0'; kprintf("%s", buf); }
    else { kprintf("(no response received)\n"); }
    libc_net_tcp_close(conn);
}
