#include "socket.h"
#include "net.h"
#include "net_internal.h"
#include "process.h"
#include "scheduler.h"
#include "string.h"
#include "printf.h"
#include "types.h"
#include "timer.h"

/* ── Socket table ────────────────────────────────────────────── */
static struct socket socket_table[SOCK_MAX];

void socket_init(void) {
    memset(socket_table, 0, sizeof(socket_table));
}

/* Convert slot to fd number (fd = slot + 100 to avoid conflict with normal fds) */
int sock_fd_from_slot(int slot) {
    return slot + 100;
}

/* Get socket struct from fd */
struct socket *sock_get(int fd) {
    int slot = fd - 100;
    if (slot < 0 || slot >= SOCK_MAX) return NULL;
    if (!socket_table[slot].in_use) return NULL;
    return &socket_table[slot];
}

/* Allocate a socket slot */
int sock_alloc(void) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!socket_table[i].in_use) {
            memset(&socket_table[i], 0, sizeof(struct socket));
            socket_table[i].in_use = 1;
            socket_table[i].state = SOCK_STATE_CREATED;
            socket_table[i].conn_id = -1;
            socket_table[i].udp_listener = -1;
            return i;
        }
    }
    return -1;
}

/* Free a socket */
void sock_free(int fd) {
    struct socket *s = sock_get(fd);
    if (!s) return;
    if (s->conn_id >= 0) net_tcp_close(s->conn_id);
    s->in_use = 0;
    s->state = SOCK_STATE_FREE;
}

/* ── Socket syscall implementations ──────────────────────────── */

int sys_socket_impl(int domain, int type, int protocol) {
    if (domain != AF_INET && domain != AF_UNIX) {
        /* Allow AF_PACKET / AF_UNSPEC for raw packet sockets */
        if (domain != 0 && domain != 17) return -1;
    }
    int slot = sock_alloc();
    if (slot < 0) return -1;

    struct socket *s = &socket_table[slot];
    s->domain = domain;
    s->type = type;
    s->protocol = protocol;
    s->rcvbuf = 65536;
    s->sndbuf = 65536;

    /* Map SOCK_STREAM → TCP, SOCK_DGRAM → UDP */
    if (protocol == 0) {
        if (type == SOCK_RAW && domain == 0) {
            /* ETH_P_ALL raw socket */
            s->protocol = ETH_P_ALL;
        } else {
            s->protocol = (type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
        }
    }

    return sock_fd_from_slot(slot);
}

int sys_bind_impl(int sockfd, const struct sockaddr_in *addr) {
    struct socket *s = sock_get(sockfd);
    if (!s) return -1;

    s->local_ip = addr->sin_addr.s_addr;
    s->local_port = ntohs(addr->sin_port);
    s->state = SOCK_STATE_BOUND;

    /* For UDP, bind the port */
    if (s->type == SOCK_DGRAM) {
        s->udp_listener = net_udp_listen(s->local_port);
        if (s->udp_listener < 0) return -1;
    }

    return 0;
}

int sys_listen_impl(int sockfd, int backlog) {
    struct socket *s = sock_get(sockfd);
    if (!s || s->state != SOCK_STATE_BOUND) return -1;
    if (s->type != SOCK_STREAM) return -1;

    s->backlog = backlog;
    s->state = SOCK_STATE_LISTENING;

    /* Register TCP listener */
    net_tcp_listen(s->local_port, NULL, NULL, NULL);
    return 0;
}

int sys_accept_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen) {
    struct socket *s = sock_get(sockfd);
    if (!s || s->state != SOCK_STATE_LISTENING) return -1;

    /* Block until a connection arrives */
    int conn_id = net_tcp_accept(s->local_port, 10000); /* 100 second timeout */
    if (conn_id < 0) return -1;

    /* Allocate a new socket for the accepted connection */
    int new_slot = sock_alloc();
    if (new_slot < 0) { net_tcp_close(conn_id); return -1; }

    struct socket *ns = &socket_table[new_slot];
    ns->domain = s->domain;
    ns->type = s->type;
    ns->protocol = s->protocol;
    ns->state = SOCK_STATE_CONNECTED;
    ns->conn_id = conn_id;
    ns->local_port = s->local_port;
    ns->local_ip = s->local_ip;

    /* Fill in peer address if requested */
    if (addr && addrlen) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(ns->remote_port);
        addr->sin_addr.s_addr = ns->remote_ip;
        *addrlen = sizeof(struct sockaddr_in);
    }

    return sock_fd_from_slot(new_slot);
}

int sys_connect_impl(int sockfd, const struct sockaddr_in *addr) {
    struct socket *s = sock_get(sockfd);
    if (!s) return -1;

    s->remote_ip = addr->sin_addr.s_addr;
    s->remote_port = ntohs(addr->sin_port);

    if (s->type == SOCK_STREAM) {
        s->conn_id = net_tcp_connect(s->remote_ip, s->remote_port);
        if (s->conn_id < 0) return -1;
        s->state = SOCK_STATE_CONNECTED;
    } else if (s->type == SOCK_DGRAM) {
        /* UDP is connectionless, but we cache the default destination */
        s->state = SOCK_STATE_CONNECTED;
    }

    return 0;
}

int sys_setsockopt_impl(int sockfd, int level, int optname,
                         const void *optval, uint32_t optlen) {
    (void)optlen;
    struct socket *s = sock_get(sockfd);
    if (!s) return -1;

    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_REUSEADDR:
                s->reuseaddr = *(int*)optval;
                return 0;
            case SO_KEEPALIVE: {
                s->keepalive = *(int*)optval;
                if (s->conn_id >= 0)
                    net_tcp_set_keepalive(s->conn_id, s->keepalive);
                return 0;
            }
            case SO_RCVBUF:
                s->rcvbuf = *(int*)optval;
                if (s->rcvbuf < 256) s->rcvbuf = 256;
                return 0;
            case SO_SNDBUF:
                s->sndbuf = *(int*)optval;
                if (s->sndbuf < 256) s->sndbuf = 256;
                return 0;
            case SO_BROADCAST:
                s->broadcast = *(int*)optval;
                return 0;
            case SO_PRIORITY:
                s->priority = *(int*)optval;
                return 0;
            case SO_MARK:
                s->sk_mark = *(uint32_t*)optval;
                return 0;
            case SO_BUSY_POLL:
                s->busy_poll_usecs = *(int*)optval;
                return 0;
            case SO_MAX_PACING_RATE:
                s->max_pacing_rate = *(uint32_t*)optval;
                return 0;
            case SO_NO_CHECK:
                s->no_check = *(int*)optval;
                return 0;
        }
    } else if (level == SOL_TCP) {
        switch (optname) {
            case TCP_NODELAY: {
                int val = *(int*)optval;
                s->tcp_nodelay = val;
                if (s->conn_id >= 0)
                    tcp_conns[s->conn_id].tcp_nodelay = val;
                return 0;
            }
            case TCP_CORK: {
                int val = *(int*)optval;
                s->tcp_cork = val;
                if (s->conn_id >= 0) {
                    struct tcp_conn *c = &tcp_conns[s->conn_id];
                    int old = c->tcp_cork;
                    c->tcp_cork = val;
                    /* Uncorking: flush buffered data */
                    if (old && !val && c->tx_unacked_len > 0) {
                        const uint8_t *p = c->tx_unacked_buf;
                        uint16_t remain = c->tx_unacked_len;
                        while (remain > 0) {
                            uint16_t chunk = remain > 1400 ? 1400 : remain;
                            send_tcp(c, TCP_PSH | TCP_ACK, p, chunk);
                            c->our_seq += chunk;
                            p += chunk;
                            remain -= chunk;
                        }
                    }
                }
                return 0;
            }
            case TCP_KEEPIDLE:
            case TCP_KEEPINTVL:
            case TCP_KEEPCNT:
                /* Keepalive tuning — store for later use if needed */
                return 0;
        }
    } else if (level == SOL_IP) {
        switch (optname) {
            case IP_TTL: {
                int val = *(int*)optval;
                s->ip_ttl = val;
                return 0;
            }
            case IP_RECVTTL: {
                s->ip_recvttl = *(int*)optval;
                return 0;
            }
            case IP_RECVDSTADDR: {
                s->ip_recvdstaddr = *(int*)optval;
                return 0;
            }
        }
    }
    return 0;
}

int sys_getsockopt_impl(int sockfd, int level, int optname,
                         void *optval, uint32_t *optlen) {
    struct socket *s = sock_get(sockfd);
    if (!s) return -1;

    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_TYPE: {
                int val = s->type;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case SO_ERROR: {
                int val = 0;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case SO_RCVBUF: {
                int val = s->rcvbuf ? s->rcvbuf : 65536;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case SO_SNDBUF: {
                int val = s->sndbuf ? s->sndbuf : 65536;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case SO_KEEPALIVE: {
                int val = s->keepalive;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case SO_REUSEADDR: {
                int val = s->reuseaddr;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case SO_PRIORITY: {
                int val = s->priority;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case SO_MARK: {
                uint32_t val = s->sk_mark;
                if (*optlen > sizeof(uint32_t)) *optlen = sizeof(uint32_t);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case SO_BUSY_POLL: {
                int val = s->busy_poll_usecs;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case SO_MAX_PACING_RATE: {
                uint32_t val = s->max_pacing_rate;
                if (*optlen > sizeof(uint32_t)) *optlen = sizeof(uint32_t);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case SO_NO_CHECK: {
                int val = s->no_check;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
        }
    } else if (level == SOL_TCP) {
        switch (optname) {
            case TCP_NODELAY: {
                int val = (s->conn_id >= 0) ? tcp_conns[s->conn_id].tcp_nodelay : s->tcp_nodelay;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case TCP_CORK: {
                int val = (s->conn_id >= 0) ? tcp_conns[s->conn_id].tcp_cork : s->tcp_cork;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case TCP_INFO: {
                struct tcp_info info;
                memset(&info, 0, sizeof(info));
                if (s->conn_id >= 0) {
                    struct tcp_conn *c = &tcp_conns[s->conn_id];
                    info.tcpi_state = (uint8_t)c->state;
                    info.tcpi_ca_state = 0;
                    info.tcpi_retransmits = c->retrans_count;
                    info.tcpi_probes = 0;
                    info.tcpi_backoff = 0;
                    info.tcpi_options = 0;
                    info.tcpi_snd_wscale = 0;
                    info.tcpi_rcv_wscale = 0;
                    info.tcpi_rto = c->rto * 10; /* convert ticks to ms */
                    info.tcpi_snd_mss = 1460;
                    info.tcpi_rcv_mss = 1460;
                    info.tcpi_unacked = c->tx_unacked_len;
                    info.tcpi_lost = 0;
                    info.tcpi_retrans = c->retrans_count;
                    info.tcpi_pmtu = 1500;
                    info.tcpi_rcv_ssthresh = c->ssthresh;
                    info.tcpi_rtt = (c->srtt > 0) ? (uint32_t)(c->srtt / 8) : 0;
                    info.tcpi_rttvar = (uint32_t)(c->rttvar / 4);
                    info.tcpi_snd_ssthresh = c->ssthresh;
                    info.tcpi_snd_cwnd = c->cwnd;
                    info.tcpi_reordering = 3;
                    info.tcpi_rcv_space = sizeof(c->rxbuf);
                    info.tcpi_total_retrans = c->retrans_count;
                } else {
                    info.tcpi_snd_cwnd = 1;
                    info.tcpi_rtt = 0;
                }
                uint32_t copylen = sizeof(info);
                if (*optlen < copylen) copylen = *optlen;
                memcpy(optval, &info, copylen);
                *optlen = copylen;
                return 0;
            }
        }
    } else if (level == SOL_IP) {
        switch (optname) {
            case IP_TTL: {
                int val = s->ip_ttl ? s->ip_ttl : 64;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case IP_MTU: {
                int val = 1500;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case IP_OPTIONS: {
                /* Return empty options */
                uint8_t empty = 0;
                if (*optlen > sizeof(uint8_t)) *optlen = sizeof(uint8_t);
                memcpy(optval, &empty, *optlen);
                return 0;
            }
            case IP_RECVTTL: {
                int val = s->ip_recvttl;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
            case IP_RECVDSTADDR: {
                int val = s->ip_recvdstaddr;
                if (*optlen > sizeof(int)) *optlen = sizeof(int);
                memcpy(optval, &val, *optlen);
                return 0;
            }
        }
    }
    return -1;
}

int sys_sendmsg_impl(int sockfd, const struct msghdr *msg, int flags) {
    (void)flags;
    struct socket *s = sock_get(sockfd);
    if (!s) return -1;

    /* For now, just write the first iovec entry */
    if (msg->msg_iovlen < 1 || !msg->msg_iov) return -1;

    uint64_t total = 0;
    for (uint32_t i = 0; i < msg->msg_iovlen; i++) {
        const void *data = msg->msg_iov[i].iov_base;
        uint64_t len = msg->msg_iov[i].iov_len;
        if (len == 0) continue;

        if (s->type == SOCK_STREAM && s->conn_id >= 0) {
            int sent = net_tcp_send(s->conn_id, data, (uint16_t)(len > 65535 ? 65535 : len));
            if (sent < 0) return total > 0 ? (int)total : -1;
            total += (uint64_t)sent;
        } else if (s->type == SOCK_DGRAM) {
            uint32_t dst_ip = s->remote_ip;
            uint16_t dst_port = s->remote_port;
            if (msg->msg_name) {
                struct sockaddr_in *dst = (struct sockaddr_in *)msg->msg_name;
                dst_ip = dst->sin_addr.s_addr;
                dst_port = ntohs(dst->sin_port);
            }
            net_udp_send(dst_ip, s->local_port, dst_port, data, (uint16_t)(len > 1500 ? 1500 : len));
            total += len;
        }
    }
    return (int)total;
}

int sys_recvmsg_impl(int sockfd, struct msghdr *msg, int flags) {
    (void)flags;
    struct socket *s = sock_get(sockfd);
    if (!s) return -1;

    if (msg->msg_iovlen < 1 || !msg->msg_iov) return -1;

    /* Receive into the first iovec buffer */
    void *buf = msg->msg_iov[0].iov_base;
    uint64_t bufsize = msg->msg_iov[0].iov_len;

    if (s->type == SOCK_STREAM && s->conn_id >= 0) {
        int n = net_tcp_recv(s->conn_id, buf, (uint16_t)(bufsize > 65535 ? 65535 : bufsize), 10);
        if (n < 0) return -1;
        return n;
    } else if (s->type == SOCK_DGRAM && s->udp_listener >= 0) {
        uint32_t src_ip;
        uint16_t src_port;
        int n = net_udp_recv((uint16_t)s->local_port, buf, (uint16_t)(bufsize > 1500 ? 1500 : bufsize),
                             &src_ip, &src_port, 10);
        if (n < 0) return -1;
        if (msg->msg_name) {
            struct sockaddr_in *src = (struct sockaddr_in *)msg->msg_name;
            src->sin_family = AF_INET;
            src->sin_port = htons(src_port);
            src->sin_addr.s_addr = src_ip;
            msg->msg_namelen = sizeof(struct sockaddr_in);
        }
        msg->msg_flags = 0;
        return n;
    }
    return -1;
}

int sys_getsockname_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen) {
    struct socket *s = sock_get(sockfd);
    if (!s) return -1;
    if (*addrlen < sizeof(struct sockaddr_in)) return -1;
    addr->sin_family = AF_INET;
    addr->sin_port = htons(s->local_port);
    addr->sin_addr.s_addr = s->local_ip;
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

int sys_getpeername_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen) {
    struct socket *s = sock_get(sockfd);
    if (!s || s->state != SOCK_STATE_CONNECTED) return -1;
    if (*addrlen < sizeof(struct sockaddr_in)) return -1;
    addr->sin_family = AF_INET;
    addr->sin_port = htons(s->remote_port);
    addr->sin_addr.s_addr = s->remote_ip;
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

int sys_socketpair_impl(int domain, int type, int protocol, int sv[2]) {
    (void)domain; (void)type; (void)protocol; (void)sv;
    /* Socketpair not yet implemented */
    return -1;
}
