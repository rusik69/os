/* nc.c — Netcat: TCP client with bidirectional stdin/connection forwarding */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: nc <host> <port>\n");
        return 1;
    }

    int ip = net_dns(argv[1]);
    if (ip < 0) {
        printf("nc: could not resolve %s\n", argv[1]);
        return 1;
    }

    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        printf("nc: invalid port %d\n", port);
        return 1;
    }

    int conn = net_tcp_connect(ip, port);
    if (conn < 0) {
        printf("nc: connection to %s:%d failed\n", argv[1], port);
        return 1;
    }

    char buf[4096];
    while (1) {
        int n = read(0, buf, sizeof(buf));
        if (n > 0) {
            net_tcp_send_conn(conn, buf, n);
        }
        int r = net_tcp_recv_conn(conn, buf, sizeof(buf));
        if (r > 0) {
            write(1, buf, r);
        }
        if (n <= 0 && r <= 0) break;
    }

    net_tcp_close_conn(conn);
    return 0;
}
