/* nc.c — Netcat: TCP/UDP connect and listen modes, port scanning.
 *
 *   nc [-l] [-u] <host> <port>     client mode (TCP by default)
 *   nc [-l] [-u] <port>            listen mode
 *   nc -z <host> <port> [endport]  TCP port scan
 *
 *   -l  listen for an inbound connection / bind a listening UDP port
 *   -u  use UDP instead of TCP
 *   -z  port scanning mode (TCP connect scan)
 *
 * TCP modes forward bytes bidirectionally between the local terminal and the
 * connection. UDP client mode sends typed input to host:port and prints any
 * replies back to a bound local port; UDP listen mode prints inbound
 * datagrams.
 */
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

#define LOCAL_UDP_PORT 12345

static void usage(void) {
    printf("Usage: nc [-l] [-u] <host> <port>          (client)\n");
    printf("       nc [-l] [-u] <port>                 (listen)\n");
    printf("       nc -z <host> <port> [endport]       (TCP scan)\n");
    printf("  -l  listen mode\n");
    printf("  -u  use UDP\n");
    printf("  -z  port scan mode\n");
}

/* Bidirectional terminal <-> conn forward; returns when the peer or the
 * local input closes. */
static void tcp_forward(int conn) {
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
        if (n <= 0 && r <= 0)
            break;
    }
}

int main(int argc, char *argv[]) {
    int listen_mode = 0, udp = 0, scan = 0;
    char *host = 0;
    char *portstr = 0;
    char *endportstr = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != 0) {
            for (const char *c = argv[i] + 1; *c; c++) {
                if (*c == 'l')
                    listen_mode = 1;
                else if (*c == 'u')
                    udp = 1;
                else if (*c == 'z')
                    scan = 1;
                else if (*c == 'h') {
                    usage();
                    return 0;
                }
            }
        } else if (!host) {
            host = argv[i];
        } else if (!portstr) {
            portstr = argv[i];
        } else if (!endportstr) {
            endportstr = argv[i];
        }
    }

    if (!portstr) {
        usage();
        return 1;
    }
    int port = atoi(portstr);
    if (port <= 0 || port > 65535) {
        printf("nc: invalid port %s\n", portstr);
        return 1;
    }

    /* Port scan mode: probe each port with a TCP connect attempt. */
    if (scan) {
        if (!host) {
            usage();
            return 1;
        }
        int ip = net_dns(host);
        if (ip < 0) {
            printf("nc: could not resolve %s\n", host);
            return 1;
        }
        int endport = port;
        if (endportstr) {
            endport = atoi(endportstr);
            if (endport <= 0 || endport > 65535 || endport < port) {
                printf("nc: invalid end port %s\n", endportstr);
                return 1;
            }
        }
        printf("nc: scanning %s ports %d-%d\n", host, port, endport);
        int open = 0;
        for (int p = port; p <= endport; p++) {
            int conn = net_tcp_connect((unsigned int)ip, p);
            if (conn >= 0) {
                printf("Port %d: open\n", p);
                open++;
                net_tcp_close_conn(conn);
            }
        }
        printf("nc: %d port(s) open\n", open);
        return 0;
    }

    if (listen_mode) {
        if (udp) {
            if (net_udp_listen(port) < 0) {
                printf("nc: could not listen on udp/%d\n", port);
                return 1;
            }
            printf("nc: listening on udp/%d\n", port);
            char buf[1500];
            unsigned int src_ip = 0;
            unsigned short src_port = 0;
            while (1) {
                int r = net_udp_recv(port, buf, sizeof(buf), &src_ip, &src_port);
                if (r > 0) {
                    printf("nc: %d.%d.%d.%d:%d: ", (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                           (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port);
                    write(1, buf, r);
                    printf("\n");
                }
            }
        }
        /* TCP listen */
        if (net_tcp_listen(port) < 0) {
            printf("nc: could not listen on tcp/%d\n", port);
            return 1;
        }
        printf("nc: listening on tcp/%d\n", port);
        int conn = net_tcp_accept(port, 0xFFFFFFFF);
        if (conn < 0) {
            printf("nc: accept failed\n");
            net_tcp_unlisten(port);
            return 1;
        }
        tcp_forward(conn);
        net_tcp_close_conn(conn);
        net_tcp_unlisten(port);
        return 0;
    }

    /* Client mode needs a host. */
    if (!host) {
        usage();
        return 1;
    }

    int ip = net_dns(host);
    if (ip < 0) {
        printf("nc: could not resolve %s\n", host);
        return 1;
    }

    if (udp) {
        if (net_udp_listen(LOCAL_UDP_PORT) < 0) {
            printf("nc: could not bind local udp port\n");
            return 1;
        }
        char buf[2048];
        unsigned int src_ip = 0;
        unsigned short src_port = 0;
        while (1) {
            int n = read(0, buf, sizeof(buf));
            if (n > 0) {
                net_udp_send((unsigned int)ip, LOCAL_UDP_PORT, (unsigned short)port, buf, n);
            }
            int r = net_udp_recv(LOCAL_UDP_PORT, buf, sizeof(buf), &src_ip, &src_port);
            if (r > 0) {
                write(1, buf, r);
            }
            if (n <= 0 && r <= 0)
                break;
        }
        net_udp_unlisten(LOCAL_UDP_PORT);
        return 0;
    }

    /* TCP client */
    int conn = net_tcp_connect((unsigned int)ip, port);
    if (conn < 0) {
        printf("nc: connection to %s:%d failed\n", host, port);
        return 1;
    }
    tcp_forward(conn);
    net_tcp_close_conn(conn);
    return 0;
}