/* socat.c — SOcket CAT: bidirectional TCP data relay.
 *
 *   socat TCP-LISTEN:<listen-port> TCP:<host>:<connect-port>
 *
 * Accepts an inbound TCP connection on listen-port and connects outbound to
 * host:connect-port, then relays bytes bidirectionally between the two using
 * fork(2) — one process pumps inbound→outbound, the other outbound→inbound.
 * This turns socat into a transparent TCP port forwarder.
 */
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

static unsigned int parse_ip(const char *s) {
    unsigned int ip = 0;
    int shift = 24;
    while (*s) {
        unsigned int val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (unsigned int)(*s - '0');
            s++;
        }
        ip |= (val << shift);
        shift -= 8;
        if (*s == '.')
            s++;
        else
            break;
    }
    return ip;
}

static int is_hostname(const char *s) {
    while (*s) {
        if (!((*s >= '0' && *s <= '9') || *s == '.'))
            return 1;
        s++;
    }
    return 0;
}

/* Pump bytes from src to dst until src reaches EOF (recv returns 0). */
static void relay(int src, int dst) {
    char buf[4096];
    while (1) {
        int n = net_tcp_recv_conn(src, buf, sizeof(buf));
        if (n <= 0)
            break;
        net_tcp_send_conn(dst, buf, n);
    }
}

static void usage(void) {
    printf("Usage: socat TCP-LISTEN:<port> TCP:<host>:<port>\n");
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage();
        return 1;
    }

    int listen_port = 0;
    char dst_host[256];
    int dst_port = 0;
    int have_listen = 0, have_connect = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "TCP-LISTEN:", 11) == 0) {
            listen_port = atoi(a + 11);
            have_listen = 1;
        } else if (strncmp(a, "TCP:", 4) == 0) {
            const char *hostpart = a + 4;
            const char *colon = strrchr(hostpart, ':');
            if (colon) {
                int hl = (int)(colon - hostpart);
                if (hl >= (int)sizeof(dst_host))
                    hl = sizeof(dst_host) - 1;
                memcpy(dst_host, hostpart, (unsigned long)hl);
                dst_host[hl] = 0;
                dst_port = atoi(colon + 1);
                have_connect = 1;
            }
        }
    }

    if (!have_listen || !have_connect || listen_port <= 0 || dst_port <= 0) {
        printf("socat: invalid or incomplete TCP endpoints\n");
        usage();
        return 1;
    }

    /* Resolve the outbound destination. */
    unsigned int dst_ip;
    if (is_hostname(dst_host)) {
        int ip = net_dns(dst_host);
        if (ip < 0) {
            printf("socat: could not resolve %s\n", dst_host);
            return 1;
        }
        dst_ip = (unsigned int)ip;
    } else {
        dst_ip = parse_ip(dst_host);
    }

    if (net_tcp_listen(listen_port) < 0) {
        printf("socat: could not listen on tcp/%d\n", listen_port);
        return 1;
    }
    printf("socat: listening on tcp/%d -> %s:%d\n", listen_port, dst_host, dst_port);

    int in = net_tcp_accept(listen_port, 0xFFFFFFFF);
    if (in < 0) {
        net_tcp_unlisten(listen_port);
        printf("socat: accept failed\n");
        return 1;
    }
    printf("socat: inbound connection accepted, dialing %s:%d\n", dst_host, dst_port);

    int out = net_tcp_connect(dst_ip, dst_port);
    if (out < 0) {
        net_tcp_close_conn(in);
        net_tcp_unlisten(listen_port);
        printf("socat: outbound connect to %s:%d failed\n", dst_host, dst_port);
        return 1;
    }

    /* Bidirectional relay: child pumps inbound->outbound, parent pumps
     * outbound->inbound. */
    int pid = fork();
    if (pid == 0) {
        relay(in, out); /* inbound -> outbound */
        exit(0);
    }

    relay(out, in); /* outbound -> inbound (parent) */
    waitpid(pid, 0, 0);

    net_tcp_close_conn(in);
    net_tcp_close_conn(out);
    net_tcp_unlisten(listen_port);
    printf("socat: relay closed\n");
    return 0;
}