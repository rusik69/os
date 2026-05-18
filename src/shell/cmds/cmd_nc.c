/* cmd_nc.c — minimal netcat: nc <host> <port> [data] */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_nc(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: nc <host> <port> [data]\n");
        shell_set_exit_status(1);
        return;
    }

    char host[64]; int port = 0; char data[256]; data[0] = '\0';
    /* parse: host port [data] */
    int i = 0;
    while (args[i] == ' ') i++;
    int j = 0;
    while (args[i] && args[i] != ' ' && j < 63) host[j++] = args[i++];
    host[j] = '\0';
    while (args[i] == ' ') i++;
    while (args[i] >= '0' && args[i] <= '9') { port = port*10 + (args[i]-'0'); i++; }
    while (args[i] == ' ') i++;
    if (args[i]) strncpy(data, args+i, 255);
    data[255] = '\0';

    if (!host[0] || port <= 0) {
        kprintf("nc: invalid host or port\n");
        shell_set_exit_status(1);
        return;
    }

    uint32_t ip = libc_net_dns_resolve(host);
    if (!ip) {
        kprintf("nc: cannot resolve '%s'\n", host);
        shell_set_exit_status(1);
        return;
    }

    int conn = libc_net_tcp_connect(ip, (uint16_t)port);
    if (conn < 0) {
        kprintf("nc: connection failed\n");
        shell_set_exit_status(1);
        return;
    }

    if (data[0]) {
        libc_net_tcp_send(conn, data, (uint16_t)strlen(data));
    }

    static char buf[1024];
    int n = libc_net_tcp_recv(conn, buf, sizeof(buf)-1, 100);
    if (n > 0) { buf[n] = '\0'; kprintf("%s", buf); }
    libc_net_tcp_close(conn);
}
