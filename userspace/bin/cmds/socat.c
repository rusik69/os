/* socat.c — SOcket CAT: TCP and UDP bidirectional data relay.
 *
 *   socat TCP-LISTEN:<listen-port> TCP:<host>:<connect-port>   TCP relay
 *   socat UDP-LISTEN:<listen-port> UDP:<host>:<dst-port>       UDP relay
 *
 * TCP: accepts an inbound connection on listen-port and connects outbound to
 * host:connect-port, then relays bytes bidirectionally via fork(2) — one
 * process pumps inbound→outbound, the other outbound→inbound.
 *
 * UDP: forwards datagrams between the local listen-port and host:dst-port in
 * both directions (a transparent UDP proxy). Inbound datagrams on the listen
 * port are forwarded to the destination; replies are received on a local
 * reply port and forwarded back to the listen port.
 *
 * NOTE: UNIX-domain socket endpoints are not yet supported — the kernel
 * socket() syscall family is not exposed to the userspace libc.
 */
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

#define UDP_REPLY_PORT 23456

enum {
    ET_NONE = 0,
    ET_TCP_SERVER, /* TCP-LISTEN:<port> -> accept()ed conn */
    ET_TCP_CLIENT, /* TCP:<host>:<port>  -> connected conn */
    ET_UDP_LISTEN, /* UDP-LISTEN:<port>  -> recv on port */
    ET_UDP_SEND,   /* UDP:<host>:<port>  -> send to dst, recv reply port */
};

struct ep {
    int kind;
    int id; /* TCP conn id or UDP local receive port */
    unsigned int dst_ip;
    int dst_port;
};

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

static void usage(void) {
    printf("Usage: socat TCP-LISTEN:<port> TCP:<host>:<port>\n");
    printf("       socat UDP-LISTEN:<port> UDP:<host>:<port>\n");
}

/* Parse a TCP or UDP address token into an endpoint descriptor (unresolved). */
static int parse_addr(const char *a, struct ep *e) {
    if (strncmp(a, "TCP-LISTEN:", 11) == 0) {
        e->kind = ET_TCP_SERVER;
        e->id = atoi(a + 11);
        return e->id > 0;
    }
    if (strncmp(a, "TCP:", 4) == 0) {
        e->kind = ET_TCP_CLIENT;
        e->dst_ip = 0;
        e->dst_port = 0;
        const char *hostpart = a + 4;
        const char *colon = strrchr(hostpart, ':');
        if (!colon)
            return 0;
        int hl = (int)(colon - hostpart);
        char host[256];
        if (hl >= (int)sizeof(host))
            hl = sizeof(host) - 1;
        memcpy(host, hostpart, (unsigned long)hl);
        host[hl] = 0;
        e->dst_port = atoi(colon + 1);
        if (e->dst_port <= 0)
            return 0;
        if (is_hostname(host)) {
            int ip = net_dns(host);
            if (ip < 0)
                return 0;
            e->dst_ip = (unsigned int)ip;
        } else {
            e->dst_ip = parse_ip(host);
        }
        return 1;
    }
    if (strncmp(a, "UDP-LISTEN:", 11) == 0) {
        e->kind = ET_UDP_LISTEN;
        e->id = atoi(a + 11);
        return e->id > 0;
    }
    if (strncmp(a, "UDP:", 4) == 0) {
        e->kind = ET_UDP_SEND;
        e->dst_ip = 0;
        e->dst_port = 0;
        const char *hostpart = a + 4;
        const char *colon = strrchr(hostpart, ':');
        if (!colon)
            return 0;
        int hl = (int)(colon - hostpart);
        char host[256];
        if (hl >= (int)sizeof(host))
            hl = sizeof(host) - 1;
        memcpy(host, hostpart, (unsigned long)hl);
        host[hl] = 0;
        e->dst_port = atoi(colon + 1);
        if (e->dst_port <= 0)
            return 0;
        if (is_hostname(host)) {
            int ip = net_dns(host);
            if (ip < 0)
                return 0;
            e->dst_ip = (unsigned int)ip;
        } else {
            e->dst_ip = parse_ip(host);
        }
        return 1;
    }
    return 0;
}

/* Read one chunk from an endpoint. For byte streams (TCP) a 0 return is EOF;
 * for UDP a 0 return is a timeout, not EOF. */
static int ep_read(const struct ep *src, void *buf, int n) {
    if (src->kind == ET_TCP_SERVER || src->kind == ET_TCP_CLIENT)
        return net_tcp_recv_conn(src->id, buf, n);
    unsigned int sip = 0;
    unsigned short sport = 0;
    return net_udp_recv((unsigned short)src->id, buf, (unsigned int)n, &sip, &sport);
}

static void ep_write(const struct ep *dst, const void *buf, int n) {
    if (dst->kind == ET_TCP_SERVER || dst->kind == ET_TCP_CLIENT) {
        net_tcp_send_conn(dst->id, buf, (unsigned int)n);
    } else {
        /* UDP: forward chunk to the destination endpoint's target. */
        int srcport = (dst->kind == ET_UDP_SEND) ? UDP_REPLY_PORT : dst->id;
        net_udp_send(dst->dst_ip, (unsigned short)srcport, (unsigned short)dst->dst_port, buf,
                     (unsigned int)n);
    }
}

/* Pump chunks from src to dst until the source reaches EOF (byte stream) */
static void pump(const struct ep *src, const struct ep *dst) {
    char buf[4096];
    while (1) {
        int n = ep_read(src, buf, (int)sizeof(buf));
        if (n < 0)
            break; /* error */
        if (n == 0) {
            if (src->kind == ET_UDP_LISTEN || src->kind == ET_UDP_SEND)
                continue; /* UDP timeout, keep relaying */
            break;        /* TCP EOF */
        }
        ep_write(dst, buf, n);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage();
        return 1;
    }

    struct ep left, right;
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    if (!parse_addr(argv[1], &left) || !parse_addr(argv[2], &right)) {
        printf("socat: invalid or incomplete endpoint: %s %s\n", argv[1], argv[2]);
        usage();
        return 1;
    }

    /* ── TCP relay: accept inbound, connect outbound, stream-relay ── */
    if (left.kind == ET_TCP_SERVER || right.kind == ET_TCP_SERVER || left.kind == ET_TCP_CLIENT ||
        right.kind == ET_TCP_CLIENT) {
        struct ep *srv = (left.kind == ET_TCP_SERVER) ? &left : &right;
        struct ep *cli = (left.kind == ET_TCP_SERVER) ? &right : &left;

        if (cli->kind != ET_TCP_CLIENT) {
            printf("socat: TCP requires one client endpoint (TCP:host:port)\n");
            usage();
            return 1;
        }

        if (net_tcp_listen((unsigned short)srv->id) < 0) {
            printf("socat: could not listen on tcp/%d\n", srv->id);
            return 1;
        }
        printf("socat: listening on tcp/%d -> %d.%d.%d.%d:%d\n", srv->id,
               (cli->dst_ip >> 24) & 0xFF, (cli->dst_ip >> 16) & 0xFF, (cli->dst_ip >> 8) & 0xFF,
               cli->dst_ip & 0xFF, cli->dst_port);

        int in = net_tcp_accept((unsigned short)srv->id, 0xFFFFFFFF);
        if (in < 0) {
            net_tcp_unlisten((unsigned short)srv->id);
            printf("socat: accept failed\n");
            return 1;
        }
        int out = net_tcp_connect(cli->dst_ip, (unsigned short)cli->dst_port);
        if (out < 0) {
            net_tcp_close_conn(in);
            net_tcp_unlisten((unsigned short)srv->id);
            printf("socat: outbound connect failed\n");
            return 1;
        }
        srv->id = in;
        cli->id = out;

        int pid = fork();
        if (pid == 0) {
            pump(srv, cli); /* inbound -> outbound */
            exit(0);
        }
        pump(cli, srv); /* outbound -> inbound */
        waitpid(pid, 0, 0);
        net_tcp_close_conn(in);
        net_tcp_close_conn(out);
        net_tcp_unlisten((unsigned short)srv->id);
        printf("socat: relay closed\n");
        return 0;
    }

    /* ── UDP relay: require one UDP-LISTEN and one UDP sender ── */
    if (left.kind == ET_UDP_LISTEN && right.kind == ET_UDP_SEND) {
        if (net_udp_listen((unsigned short)left.id) < 0) {
            printf("socat: could not listen on udp/%d\n", left.id);
            return 1;
        }
        if (net_udp_listen((unsigned short)UDP_REPLY_PORT) < 0) {
            net_udp_unlisten((unsigned short)left.id);
            printf("socat: could not bind reply port\n");
            return 1;
        }
        printf("socat: udp relay udp/%d -> %d.%d.%d.%d:%d\n", left.id, (right.dst_ip >> 24) & 0xFF,
               (right.dst_ip >> 16) & 0xFF, (right.dst_ip >> 8) & 0xFF, right.dst_ip & 0xFF,
               right.dst_port);

        /* child: forward inbound on listen-port -> destination */
        int pid = fork();
        if (pid == 0) {
            pump(&left, &right);
            exit(0);
        }
        /* parent: forward replies from destination -> listen port */
        struct ep reply;
        reply.kind = ET_UDP_SEND;
        reply.id = UDP_REPLY_PORT;
        reply.dst_ip = 0x7F000001; /* loopback: reply back to local listen */
        reply.dst_port = left.id;
        pump(&reply, &left);
        waitpid(pid, 0, 0);
        net_udp_unlisten((unsigned short)left.id);
        net_udp_unlisten((unsigned short)UDP_REPLY_PORT);
        return 0;
    }

    printf("socat: unsupported endpoint combination\n");
    usage();
    return 1;
}