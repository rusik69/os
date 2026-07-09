#include "socket.h"
#include "net.h"
#include "net_internal.h"
#include "process.h"
#include "scheduler.h"
#include "string.h"
#include "printf.h"
#include "poll.h"
#include "can.h"
#include "types.h"
#include "timer.h"
#include "export.h"
#include "module.h"      /* request_module() for protocol autoloading */
#include "errno.h"
#include "af_packet.h"    /* AF_PACKET raw packet sockets (Item 386) */
#include "netlink.h"       /* AF_NETLINK kernel-userspace sockets (Item 384) */
#include "can.h"           /* AF_CAN SocketCAN protocol (Item 352) */

/* ── Compile-time struct size assertions ────────────────────────────── */
_Static_assert(sizeof(struct socket) >= 64, "struct socket must be at least 64 bytes for fixed-size table");
_Static_assert(sizeof(struct sockaddr_in) == 16, "sockaddr_in must be 16 bytes (ABI)");
_Static_assert(sizeof(struct tcp_info) == 104, "tcp_info must be 104 bytes (ABI)");

/* ── Socket table ────────────────────────────────────────────── */
static struct socket socket_table[SOCK_MAX];

void socket_init(void) {
    memset(socket_table, 0, sizeof(socket_table));
    af_unix_init();
    af_packet_init();
    af_netlink_init();
    can_init();
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
            wait_queue_init(&socket_table[i].wq);
            return i;
        }
    }
    return -ENOMEM;
}

/* Free a socket */
void sock_free(int fd) {
    struct socket *s = sock_get(fd);
    if (!s) return;
    /* Destroy AF_UNIX endpoint if present */
    if (s->domain == AF_UNIX && s->unix_ep >= 0) {
        unix_destroy(s->unix_ep);
        s->unix_ep = -1;
    }
    /* Destroy AF_PACKET socket if present */
    if (s->domain == AF_PACKET || (s->domain == 0 && s->type == SOCK_RAW)) {
        packet_close(fd);
    }
    /* Destroy AF_NETLINK socket if present */
    if (s->domain == AF_NETLINK) {
        netlink_close(fd);
    }
    /* Destroy AF_CAN socket if present */
    if (s->domain == AF_CAN) {
        can_close(fd);
    }
    if (s->conn_id >= 0) net_tcp_close(s->conn_id);
    s->in_use = 0;
    s->state = SOCK_STATE_FREE;
}

/* ── Socket syscall implementations ──────────────────────────── */

int sys_socket_impl(int domain, int type, int protocol) {
    /* ── Network protocol module autoloading (M37) ─────────────────
     * When an unsupported address family is requested (e.g. AF_INET6),
     * attempt to autoload the corresponding protocol module before
     * giving up.  This allows IPv6, IPIP, GRE and other protocol
     * modules to be loaded on demand.
     */
    if (domain == AF_INET6) {
        request_module("ipv6");
        /* After module load, check if IPv6 is now available.
         * If not, we fall through to the existing error path. */
    }

    if (domain != AF_INET && domain != AF_INET6 && domain != AF_UNIX) {
        /* Allow AF_PACKET / AF_UNSPEC for raw packet sockets */
        if (domain != 0 && domain != 17 && domain != AF_NETLINK && domain != AF_CAN) return -EAFNOSUPPORT;
    }

    /* Validate socket type for AF_NETLINK — Linux only allows
     * SOCK_RAW (direct message access) or SOCK_DGRAM for netlink. */
    if (domain == AF_NETLINK && type != SOCK_RAW && type != SOCK_DGRAM)
        return -EPROTONOSUPPORT;

    int slot = sock_alloc();
    if (slot < 0) return slot; /* -ENOMEM */

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
        } else if (domain == AF_NETLINK) {
            /* AF_NETLINK uses netlink protocol families (NETLINK_ROUTE,
             * NETLINK_GENERIC, etc.) — never map to TCP/UDP.  The
             * netlink_create() call below will default to NETLINK_GENERIC
             * when protocol remains 0. */
            s->protocol = 0;
        } else {
            s->protocol = (type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
        }
    }

    /* For AF_UNIX: create a local endpoint */
    s->unix_ep = -1;
    if (domain == AF_UNIX) {
        int ep = unix_create(type);
        if (ep < 0) { sock_free(sock_fd_from_slot(slot)); return -EINVAL; }
        s->unix_ep = ep;
    }

    /* For AF_PACKET: create a raw packet socket endpoint */
    if ((domain == AF_PACKET || domain == 0) && type == SOCK_RAW) {
        int ret = packet_create(sock_fd_from_slot(slot), type,
                                (uint16_t)s->protocol);
        if (ret < 0) { sock_free(sock_fd_from_slot(slot)); return -EINVAL; }
    }

    /* For AF_NETLINK: create a netlink socket endpoint */
    if (domain == AF_NETLINK) {
        int proto = (int)protocol;
        if (proto < 0) proto = NETLINK_GENERIC; /* Default protocol */
        int ret = netlink_create(sock_fd_from_slot(slot), proto);
        if (ret < 0) { sock_free(sock_fd_from_slot(slot)); return -EINVAL; }
    }

    /* For AF_CAN: create a CAN bus socket endpoint (Item 352) */
    if (domain == AF_CAN) {
        int can_proto = (int)protocol;
        if (can_proto <= 0) can_proto = CAN_RAW; /* Default to RAW */
        int ret = can_create(can_proto);
        if (ret < 0) { sock_free(sock_fd_from_slot(slot)); return -EINVAL; }
    }

    return sock_fd_from_slot(slot);
}

int sys_bind_impl(int sockfd, const struct sockaddr_in *addr, int addrlen)
{
    struct socket *s = sock_get(sockfd);
    if (!s) return -EBADF;

    /* Validate minimum address length (at least the family field) */
    if (addrlen < (int)sizeof(uint16_t))
        return -EINVAL;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX) {
        if (addrlen < (int)sizeof(struct sockaddr_un))
            return -EINVAL;
        if (s->unix_ep < 0) return -EINVAL;
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        int ret = unix_bind(s->unix_ep, un, (uint32_t)addrlen);
        if (ret == 0) s->state = SOCK_STATE_BOUND;
        return ret;
    }

    /* AF_PACKET: dispatch to raw packet handler */
    if (s->domain == AF_PACKET || (s->domain == 0 && s->type == SOCK_RAW)) {
        if (addrlen < (int)sizeof(struct sockaddr_ll))
            return -EINVAL;
        /* sockaddr_ll structure cast from addr */
        const struct sockaddr_ll *sll = (const struct sockaddr_ll *)addr;
        /* Bind to interface index (0 = any interface) */
        int ret = packet_bind(sockfd, (int)sll->sll_ifindex);
        if (ret == 0) s->state = SOCK_STATE_BOUND;
        return ret;
    }

    /* AF_NETLINK: dispatch to netlink handler */
    if (s->domain == AF_NETLINK) {
        if (addrlen < (int)sizeof(struct sockaddr_nl))
            return -EINVAL;
        const struct sockaddr_nl *nl_addr = (const struct sockaddr_nl *)addr;
        int ret = netlink_bind(sockfd, nl_addr);
        if (ret == 0) s->state = SOCK_STATE_BOUND;
        return ret;
    }

    /* AF_CAN: dispatch to CAN bus handler */
    if (s->domain == AF_CAN) {
        if (addrlen < (int)sizeof(struct sockaddr_can))
            return -EINVAL;
        const struct sockaddr_can *can_addr = (const struct sockaddr_can *)addr;
        int ret = can_bind(sockfd, can_addr);
        if (ret == 0) s->state = SOCK_STATE_BOUND;
        return ret;
    }

    /* AF_INET (or default) — sockaddr_in */
    if (addrlen < (int)sizeof(struct sockaddr_in))
        return -EINVAL;
    s->local_ip = addr->sin_addr.s_addr;
    s->local_port = ntohs(addr->sin_port);
    s->state = SOCK_STATE_BOUND;

    /* For UDP, bind the port */
    if (s->type == SOCK_DGRAM) {
        s->udp_listener = net_udp_listen(s->local_port);
        if (s->udp_listener < 0) return -EADDRINUSE;
    }

    return 0;
}

int sys_listen_impl(int sockfd, int backlog) {
    struct socket *s = sock_get(sockfd);
    if (!s) return -EBADF;
    if (s->state != SOCK_STATE_BOUND) return -EINVAL;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX) {
        if (s->unix_ep < 0) return -EINVAL;
        int ret = unix_listen(s->unix_ep, backlog);
        if (ret == 0) s->state = SOCK_STATE_LISTENING;
        return ret;
    }

    s->backlog = backlog;
    s->state = SOCK_STATE_LISTENING;

    /* Register TCP listener */
    net_tcp_listen(s->local_port, NULL, NULL, NULL);
    return 0;
}

int sys_accept_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen) {
    struct socket *s = sock_get(sockfd);
    if (!s) return -EBADF;
    if (s->state != SOCK_STATE_LISTENING) return -EINVAL;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX) {
        if (s->unix_ep < 0) return -EINVAL;
        int client_ep = unix_accept(s->unix_ep, 0);
        if (client_ep < 0) return -EINVAL;

        /* Allocate a new socket for the accepted connection */
        int new_slot = sock_alloc();
        if (new_slot < 0) { unix_destroy(client_ep); return -ENOMEM; }

        struct socket *ns = &socket_table[new_slot];
        ns->domain = AF_UNIX;
        ns->type = s->type;
        ns->protocol = 0;
        ns->state = SOCK_STATE_CONNECTED;
        ns->unix_ep = client_ep;

        /* Fill in peer address if requested — return empty AF_UNIX addr */
        if (addr && addrlen) {
            struct sockaddr_un *un = (struct sockaddr_un *)addr;
            un->sun_family = AF_UNIX;
            un->sun_path[0] = '\0';
            *addrlen = (uint32_t)sizeof(uint16_t);
        }
        return sock_fd_from_slot(new_slot);
    }

    /* Block until a connection arrives */
    int conn_id = net_tcp_accept(s->local_port, 10000); /* 100 second timeout */
    if (conn_id < 0) return -ETIMEDOUT;

    /* Allocate a new socket for the accepted connection */
    int new_slot = sock_alloc();
    if (new_slot < 0) { net_tcp_close(conn_id); return -ENOMEM; }

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
    if (!s) return -EBADF;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX) {
        if (s->unix_ep < 0) return -EINVAL;
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        int ret = unix_connect(s->unix_ep, un, sizeof(struct sockaddr_un));
        if (ret == 0) s->state = SOCK_STATE_CONNECTED;
        return ret;
    }

    s->remote_ip = addr->sin_addr.s_addr;
    s->remote_port = ntohs(addr->sin_port);

    if (s->type == SOCK_STREAM) {
        s->conn_id = net_tcp_connect(s->remote_ip, s->remote_port);
        if (s->conn_id < 0) return -ECONNREFUSED;
        s->state = SOCK_STATE_CONNECTED;
    } else if (s->type == SOCK_DGRAM) {
        /* UDP is connectionless, but we cache the default destination */
        s->state = SOCK_STATE_CONNECTED;

        /* Pre-resolve the MAC route for the connected UDP fast path.
         * If ARP already has the entry, cache it so subsequent sends
         * can bypass the ARP cache lookup.  If unresolved, the cache
         * stays invalid and the normal send path is used instead. */
        uint8_t *mac = arp_cache_lookup(s->remote_ip);
        if (mac) {
            memcpy(s->cached_dst_mac, mac, 6);
            s->cache_valid = 1;
        } else {
            s->cache_valid = 0;
        }
    }

    return 0;
}

int sys_setsockopt_impl(int sockfd, int level, int optname,
                         const void *optval, uint32_t optlen) {
    struct socket *s = sock_get(sockfd);
    if (!s) return -EBADF;

    /* Validate optlen — must be at least sizeof(int) for integer options */
    if (!optval || optlen < sizeof(int))
        return -EINVAL;

    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_REUSEADDR:
                s->reuseaddr = *(const int*)optval;
                return 0;
            case SO_KEEPALIVE: {
                s->keepalive = *(const int*)optval;
                if (s->conn_id >= 0)
                    net_tcp_set_keepalive(s->conn_id, s->keepalive);
                return 0;
            }
            case SO_RCVBUF:
                s->rcvbuf = *(const int*)optval;
                if (s->rcvbuf < 256) s->rcvbuf = 256;
                return 0;
            case SO_SNDBUF:
                s->sndbuf = *(const int*)optval;
                if (s->sndbuf < 256) s->sndbuf = 256;
                return 0;
            case SO_BROADCAST:
                s->broadcast = *(const int*)optval;
                return 0;
            case SO_PRIORITY:
                s->priority = *(const int*)optval;
                return 0;
            case SO_MARK:
                s->sk_mark = *(const uint32_t*)optval;
                return 0;
            case SO_BUSY_POLL:
                s->busy_poll_usecs = *(const int*)optval;
                return 0;
            case SO_MAX_PACING_RATE:
                s->max_pacing_rate = *(const uint32_t*)optval;
                return 0;
            case SO_NO_CHECK:
                s->no_check = *(const int*)optval;
                return 0;
            case SO_RCVTIMEO: {
                const struct timeval *tv = (const struct timeval *)optval;
                s->busy_poll_usecs = (int)(tv->tv_sec * 1000000 + tv->tv_usec);
                return 0;
            }
            case SO_SNDTIMEO: {
                const struct timeval *tv = (const struct timeval *)optval;
                s->max_pacing_rate = (uint32_t)(tv->tv_sec * 1000000 + tv->tv_usec);
                return 0;
            }
            default:
                break;
        }
    } else if (level == SOL_TCP) {
        switch (optname) {
            case TCP_NODELAY: {
                int val = *(const int*)optval;
                s->tcp_nodelay = val;
                if (s->conn_id >= 0)
                    tcp_conns[s->conn_id].tcp_nodelay = val;
                return 0;
            }
            case TCP_CORK: {
                int val = *(const int*)optval;
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
            default:
                break;
        }
    } else if (level == SOL_IP) {
        switch (optname) {
            case IP_TTL: {
                int val = *(const int*)optval;
                s->ip_ttl = val;
                return 0;
            }
            case IP_RECVTTL: {
                s->ip_recvttl = *(const int*)optval;
                return 0;
            }
            case IP_RECVDSTADDR: {
                s->ip_recvdstaddr = *(const int*)optval;
                return 0;
            }
            case IP_FREEBIND: {
                s->broadcast = *(const int*)optval;
                return 0;
            }
            default:
                break;
        }
    } else if (level == SOL_CAN_RAW || level == SOL_CAN_BASE) {
        /* AF_CAN: socket options */
        if (s->domain == AF_CAN) {
            return can_setsockopt(sockfd, level, optname, optval, optlen);
        }
    }
    return 0;
}

int sys_getsockopt_impl(int sockfd, int level, int optname,
                         void *optval, uint32_t *optlen) {
    struct socket *s = sock_get(sockfd);
    if (!s) return -EBADF;

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
            default:
                break;
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
            default:
                break;
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
            default:
                break;
        }
    }
    return -EINVAL;
}

int sys_sendmsg_impl(int sockfd, const struct msghdr *msg, int flags) {
    (void)flags;
    struct socket *s = sock_get(sockfd);
    if (!s) return -EBADF;

    /* For now, just write the first iovec entry */
    if (msg->msg_iovlen < 1 || !msg->msg_iov) return -EINVAL;

    /* AF_NETLINK: use msghdr-aware sendmsg that flattens all iovecs
     * into one contiguous netlink message. */
    if (s->domain == AF_NETLINK) {
        if (!netlink_is_valid_fd(sockfd))
            return -EINVAL;
        return netlink_sendmsg(sockfd, msg, flags);
    }

    uint64_t total = 0;
    for (uint32_t i = 0; i < msg->msg_iovlen; i++) {
        const void *data = msg->msg_iov[i].iov_base;
        uint64_t len = msg->msg_iov[i].iov_len;
        if (len == 0) continue;

        /* AF_UNIX: dispatch to local socket handler using sendmsg */
        if (s->domain == AF_UNIX && s->unix_ep >= 0) {
            int sent = unix_sendmsg(s->unix_ep, msg, flags);
            if (sent < 0) return sent;
            return sent;
        } else if ((s->domain == AF_PACKET || (s->domain == 0 && s->type == SOCK_RAW)) &&
                   packet_is_valid_fd(sockfd)) {
            /* AF_PACKET raw packet send */
            int sent = packet_send(sockfd, data, (int)(len > 2048 ? 2048 : len));
            if (sent < 0) return total > 0 ? (int)total : -EIO;
            total += (uint64_t)sent;
        } else if (s->domain == AF_CAN) {
            /* AF_CAN: send CAN frame */
            if (len < sizeof(struct can_frame)) return -EINVAL;
            int sent = can_send(sockfd, (const struct can_frame *)data);
            if (sent < 0) return total > 0 ? (int)total : -EIO;
            total += (uint64_t)sent;
        } else if (s->type == SOCK_STREAM && s->conn_id >= 0) {
            int sent = net_tcp_send(s->conn_id, data, (uint16_t)(len > 65535 ? 65535 : len));
            if (sent < 0) return total > 0 ? (int)total : -EIO;
            total += (uint64_t)sent;
        } else if (s->type == SOCK_DGRAM) {
            uint32_t dst_ip = s->remote_ip;
            uint16_t dst_port = s->remote_port;
            if (msg->msg_name) {
                struct sockaddr_in *dst = (struct sockaddr_in *)msg->msg_name;
                dst_ip = dst->sin_addr.s_addr;
                dst_port = ntohs(dst->sin_port);
                /* Explicit destination — invalidate route cache since
                 * the next send may target a different peer. */
                s->cache_valid = 0;
            }
            uint64_t udp_len = len > 1500 ? 1500 : len;
            /* Connected UDP fast path: use pre-resolved MAC to skip
             * ARP cache lookup inside send_ip(). */
            if (s->cache_valid && s->state == SOCK_STATE_CONNECTED && dst_ip == s->remote_ip) {
                net_udp_send_cached(s->cached_dst_mac, dst_ip,
                                    s->local_port, dst_port, data,
                                    (uint16_t)udp_len);
            } else {
                net_udp_send(dst_ip, s->local_port, dst_port, data,
                             (uint16_t)udp_len);
            }
            total += udp_len;
        }
    }
    /* Clamp total to INT32_MAX to avoid signed overflow on return.
     * sendmsg(2) returns ssize_t; this implementation returns int, so
     * values above INT32_MAX cannot be represented correctly. */
    if (total > 0x7FFFFFFFULL)
        total = 0x7FFFFFFFULL;
    return (int)total;
}

int sys_recvmsg_impl(int sockfd, struct msghdr *msg, int flags) {
    (void)flags;
    struct socket *s = sock_get(sockfd);
    if (!s) return -EBADF;

    if (msg->msg_iovlen < 1 || !msg->msg_iov) return -EINVAL;

    /* Receive into the first iovec buffer */
    void *buf = msg->msg_iov[0].iov_base;
    uint64_t bufsize = msg->msg_iov[0].iov_len;

    /* AF_UNIX: dispatch to local socket handler using recvmsg */
    if (s->domain == AF_UNIX && s->unix_ep >= 0) {
        int n = unix_recvmsg(s->unix_ep, msg, flags);
        if (n <= 0) return -EINVAL;
        return n;
    }

    /* AF_PACKET: raw packet receive */
    if ((s->domain == AF_PACKET || (s->domain == 0 && s->type == SOCK_RAW)) &&
        packet_is_valid_fd(sockfd)) {
        uint64_t ifindex = 0;
        int n = packet_recv(sockfd, buf, (int)(bufsize > 2048 ? 2048 : bufsize), &ifindex);
        if (n < 0) return n; /* propagate -EAGAIN, -EIO, etc. */
        if (msg->msg_name && n >= 0) {
            struct sockaddr_ll *sll = (struct sockaddr_ll *)msg->msg_name;
            memset(sll, 0, sizeof(struct sockaddr_ll));
            sll->sll_family  = AF_PACKET;
            sll->sll_ifindex = (int)ifindex;
            sll->sll_halen   = 6; /* Ethernet */
            msg->msg_namelen = sizeof(struct sockaddr_ll);
        }
        return n;
    }

    /* AF_NETLINK: use msghdr-aware recvmsg */
    if (s->domain == AF_NETLINK) {
        if (!netlink_is_valid_fd(sockfd))
            return -EINVAL;
        return netlink_recvmsg(sockfd, msg, flags);
    }

    /* AF_CAN: CAN frame receive */
    if (s->domain == AF_CAN) {
        if (bufsize < sizeof(struct can_frame)) return -EINVAL;
        int n = can_recv(sockfd, (struct can_frame *)buf);
        if (n < 0) return -EINVAL;
        if (msg->msg_name && n >= 0) {
            struct sockaddr_can *scan = (struct sockaddr_can *)msg->msg_name;
            memset(scan, 0, sizeof(struct sockaddr_can));
            scan->can_family = AF_CAN;
            msg->msg_namelen = sizeof(struct sockaddr_can);
        }
        return n;
    }

    if (s->type == SOCK_STREAM && s->conn_id >= 0) {
        int n = net_tcp_recv(s->conn_id, buf, (uint16_t)(bufsize > 65535 ? 65535 : bufsize), 10);
        if (n < 0) return -EINVAL;
        return n;
    } else if (s->type == SOCK_DGRAM && s->udp_listener >= 0) {
        uint32_t src_ip;
        uint16_t src_port;
        int n = net_udp_recv((uint16_t)s->local_port, buf, (uint16_t)(bufsize > 1500 ? 1500 : bufsize),
                             &src_ip, &src_port, 10);
        if (n < 0) return -EINVAL;
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
    return -EINVAL;
}

int sys_getsockname_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen) {
    struct socket *s = sock_get(sockfd);
    if (!s) return -EBADF;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX && s->unix_ep >= 0) {
        int ret = unix_getsockname(s->unix_ep, (struct sockaddr_un *)addr, addrlen);
        return (ret == 0) ? 0 : -EINVAL;
    }

    /* AF_CAN: dispatch to CAN getsockname */
    if (s->domain == AF_CAN) {
        if (*addrlen < sizeof(struct sockaddr_can)) return -EINVAL;
        struct sockaddr_can *can_addr = (struct sockaddr_can *)addr;
        int ret = can_getsockname(sockfd, can_addr);
        if (ret == 0) *addrlen = sizeof(struct sockaddr_can);
        return (ret == 0) ? 0 : -EOPNOTSUPP;
    }

    /* AF_PACKET: dispatch to raw packet getsockname */
    if (s->domain == AF_PACKET || (s->domain == 0 && s->type == SOCK_RAW)) {
        if (*addrlen < sizeof(struct sockaddr_ll)) return -EINVAL;
        struct sockaddr_ll *sll = (struct sockaddr_ll *)addr;
        int ret = packet_getsockname(sockfd, sll);
        if (ret == 0) *addrlen = sizeof(struct sockaddr_ll);
        return (ret == 0) ? 0 : -EOPNOTSUPP;
    }

    if (*addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    addr->sin_family = AF_INET;
    addr->sin_port = htons(s->local_port);
    addr->sin_addr.s_addr = s->local_ip;
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

int sys_getpeername_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen) {
    struct socket *s = sock_get(sockfd);
    if (!s) return -EBADF;
    if (s->state != SOCK_STATE_CONNECTED) return -ENOTCONN;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX && s->unix_ep >= 0) {
        int ret = unix_getpeername(s->unix_ep, (struct sockaddr_un *)addr, addrlen);
        return (ret == 0) ? 0 : -EINVAL;
    }

    if (*addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    addr->sin_family = AF_INET;
    addr->sin_port = htons(s->remote_port);
    addr->sin_addr.s_addr = s->remote_ip;
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

int sys_socketpair_impl(int domain, int type, int protocol, int sv[2]) {
    (void)protocol;

    /* AF_UNIX socket pairs */
    if (domain == AF_UNIX) {
        /* Support SOCK_STREAM and SOCK_DGRAM and SOCK_SEQPACKET */
        if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET)
            return -EOPNOTSUPP;

        /* Allocate two socket slots */
        int slot0 = sock_alloc();
        if (slot0 < 0) return -ENOMEM;

        int slot1 = sock_alloc();
        if (slot1 < 0) {
            sock_free(sock_fd_from_slot(slot0));
            return -ENOMEM;
        }

        /* Create a connected AF_UNIX pair */
        int ep0, ep1;
        int ret = unix_socketpair(&ep0, &ep1);
        if (ret < 0) {
            sock_free(sock_fd_from_slot(slot0));
            sock_free(sock_fd_from_slot(slot1));
            return ret;
        }

        /* Set up socket 0 */
        struct socket *s0 = &socket_table[slot0];
        s0->domain  = AF_UNIX;
        s0->type    = type;
        s0->state   = SOCK_STATE_CONNECTED;
        s0->unix_ep = ep0;

        /* Set up socket 1 */
        struct socket *s1 = &socket_table[slot1];
        s1->domain  = AF_UNIX;
        s1->type    = type;
        s1->state   = SOCK_STATE_CONNECTED;
        s1->unix_ep = ep1;

        sv[0] = sock_fd_from_slot(slot0);
        sv[1] = sock_fd_from_slot(slot1);
        return 0;
    }

    /* AF_INET socketpair: create a TCP loopback pair */
    if (domain == AF_INET && type == SOCK_STREAM) {
        /* Allocate two socket slots */
        int slot0 = sock_alloc();
        if (slot0 < 0) return -ENOMEM;

        struct socket *s0 = &socket_table[slot0];
        s0->domain  = AF_INET;
        s0->type    = SOCK_STREAM;
        s0->protocol = IPPROTO_TCP;
        s0->state   = SOCK_STATE_BOUND;
        s0->local_port = 0; /* ephemeral */

        /* Bind to a random port */
        s0->local_port = (uint16_t)(30000 + ((uint32_t)(uintptr_t)s0 ^ (uint32_t)timer_get_ticks()) % 10000);
        s0->local_ip = htonl(0x7F000001); /* 127.0.0.1 */

        /* Listen */
        s0->state = SOCK_STATE_LISTENING;
        net_tcp_listen(s0->local_port, NULL, NULL, NULL);

        /* Allocate slot 1 */
        int slot1 = sock_alloc();
        if (slot1 < 0) {
            sock_free(sock_fd_from_slot(slot0));
            return -ENOMEM;
        }

        struct socket *s1 = &socket_table[slot1];
        s1->domain  = AF_INET;
        s1->type    = SOCK_STREAM;
        s1->protocol = IPPROTO_TCP;

        /* Connect to slot 0 */
        s1->remote_ip = htonl(0x7F000001);
        s1->remote_port = s0->local_port;
        s1->conn_id = net_tcp_connect(s1->remote_ip, s1->remote_port);
        if (s1->conn_id < 0) {
            sock_free(sock_fd_from_slot(slot0));
            sock_free(sock_fd_from_slot(slot1));
            return -EINVAL;
        }
        s1->state = SOCK_STATE_CONNECTED;

        /* Accept on slot 0 */
        int conn_id = net_tcp_accept(s0->local_port, 100);
        if (conn_id < 0) {
            net_tcp_close(s1->conn_id);
            sock_free(sock_fd_from_slot(slot0));
            sock_free(sock_fd_from_slot(slot1));
            return -EINVAL;
        }

        s0->conn_id = conn_id;
        s0->state = SOCK_STATE_CONNECTED;

        sv[0] = sock_fd_from_slot(slot0);
        sv[1] = sock_fd_from_slot(slot1);
        return 0;
    }

    return -EOPNOTSUPP;
}

/* ── Socket poll support ─────────────────────────────────────── */

int sock_poll(int sockfd, int events, struct poll_table *pt)
{
    struct socket *s = sock_get(sockfd);
    if (!s) return POLLNVAL;

    int revents = 0;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX && s->unix_ep >= 0) {
        return unix_poll(s->unix_ep, events);
    }

    /* AF_CAN: dispatch to CAN poll handler */
    if (s->domain == AF_CAN) {
        revents = can_poll(sockfd);
        return revents & events;
    }

    switch (s->type) {
        case SOCK_STREAM: {
            /* ── TCP / stream socket ────────────────────────── */
            if (s->state == SOCK_STATE_LISTENING) {
                /* Listening socket: readable if accept queue has connections.
                 * Use the listener's accept_count. We look up the listener
                 * by iterating net_listeners (declared in net_internal.h).
                 * For now, a simpler approach: check if accept_count > 0
                 * by trying to peek at the listener state via net_tcp_get_info.
                 * Since we don't have direct access to the listener table
                 * from socket.c, we return POLLIN optimistically and handle
                 * it in sys_accept_impl (which will block if nothing pending). */
                /* On a listening socket, POLLIN means a connection is pending.
                 * Since we can't easily peek at the accept queue from here,
                 * we always report POLLIN — the accept() call will block
                 * if nothing is available. */
                revents |= POLLOUT; /* listening sockets can accept new connections */
                if (events & POLLIN) revents |= POLLIN;
            } else if (s->state == SOCK_STATE_CONNECTED && s->conn_id >= 0) {
                /* Connected stream socket */
                /* POLLIN: data available or FIN received (EOF) */
                if (events & POLLIN) {
                    if (net_tcp_available(s->conn_id) > 0 || net_tcp_has_closed(s->conn_id))
                        revents |= POLLIN;
                }
                /* POLLOUT: connected and writable (buffer space available) */
                if (events & POLLOUT) {
                    if (net_tcp_is_connected(s->conn_id))
                        revents |= POLLOUT;
                }
                /* POLLHUP: connection closed */
                if (net_tcp_has_closed(s->conn_id))
                    revents |= POLLHUP;
            } else if (s->state == SOCK_STATE_CONNECTING) {
                /* Socket is in the process of connecting — not yet
                 * readable or writable. POLLOUT will fire when connected.
                 * For now, never report ready — the caller will poll again. */
                /* Could add a check here if connect completed */
            } else {
                /* Not connected: POLLHUP */
                revents |= POLLHUP;
            }
            break;
        }

        case SOCK_DGRAM: {
            /* ── UDP / datagram socket ───────────────────────── */
            /* POLLOUT: UDP is always writable (no connection state) */
            if (events & POLLOUT)
                revents |= POLLOUT;
            /* POLLIN: data may be available; we optimistically report
             * POLLIN if bound (listening on a port). The recvmsg()
             * call will block or return -EAGAIN if no data. */
            if (events & POLLIN && s->udp_listener >= 0)
                revents |= POLLIN;
            if (s->state == SOCK_STATE_CONNECTED) {
                /* Connected UDP: also report POLLIN optimistically */
                if (events & POLLIN)
                    revents |= POLLIN;
                /* POLLOUT already set above */
            }
            break;
        }

        default:
            /* Unknown socket type */
            revents = POLLERR;
            break;
    }

    /* Mask with requested events — only report what was asked for */
    int result = revents & events;

    /* If nothing is ready and we have a poll_table, register the
     * socket's waitqueue so that poll_schedule can block on it
     * and wake when data arrives or state changes. */
    if (result == 0 && pt)
        poll_wait(pt, &s->wq);

    return result;
}

/* ── Exported symbols for network protocol/driver modules ─────────── */
EXPORT_SYMBOL(socket_init);
EXPORT_SYMBOL(sock_get);
EXPORT_SYMBOL(sock_alloc);
EXPORT_SYMBOL(sock_free);
EXPORT_SYMBOL(sys_socket_impl);
EXPORT_SYMBOL(sys_bind_impl);
EXPORT_SYMBOL(sys_connect_impl);
EXPORT_SYMBOL(sys_listen_impl);
EXPORT_SYMBOL(sys_accept_impl);
EXPORT_SYMBOL(sys_sendmsg_impl);
EXPORT_SYMBOL(sys_recvmsg_impl);
/* ── Wake socket by connection ID ────────────────────────────── */

/*
 * sock_wake_by_conn_id — wake the waitqueue of all sockets that
 * have the given conn_id.  Called from the TCP stack when data
 * arrives so poll/select/epoll waiters wake up and re-check.
 */
void sock_wake_by_conn_id(int conn_id)
{
    for (int i = 0; i < SOCK_MAX; i++) {
        if (socket_table[i].in_use &&
            socket_table[i].conn_id == conn_id) {
            wait_queue_wake_all(&socket_table[i].wq);
        }
    }
}
EXPORT_SYMBOL(sys_setsockopt_impl);
EXPORT_SYMBOL(sys_getsockopt_impl);

/* Export socket poll for protocol modules */
EXPORT_SYMBOL(sock_poll);

/* ── Implement: socket_create ─────────────────────────── */
static int socket_create(int family, int type, int proto)
{
    return sys_socket_impl(family, type, proto);
}
/* ── Implement: socket_bind ───────────────────────────── */
static int socket_bind(int sock, const void *addr, int addrlen)
{
    return sys_bind_impl(sock, (const struct sockaddr_in *)addr, addrlen);
}
/* ── Implement: socket_listen ─────────────────────────── */
static int socket_listen(int sock, int backlog)
{
    return sys_listen_impl(sock, backlog);
}
/* ── Implement: socket_accept ─────────────────────────── */
static int socket_accept(int sock, void *addr, void *addrlen)
{
    return sys_accept_impl(sock, (struct sockaddr_in *)addr, (uint32_t *)addrlen);
}
