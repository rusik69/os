#ifndef SOCKET_H
#define SOCKET_H

#include "types.h"
#include "net.h" /* for net_tcp_* API */

/* ── Poll event flags ────────────────────────────────────────── */
#ifndef POLLIN
#define POLLIN     0x001
#endif
#ifndef POLLOUT
#define POLLOUT    0x004
#endif
#ifndef POLLERR
#define POLLERR    0x008
#endif
#ifndef POLLHUP
#define POLLHUP    0x010
#endif
#ifndef POLLNVAL
#define POLLNVAL   0x020
#endif

/* ── Socket types ───────────────────────────────────────────── */
#define AF_UNSPEC       0
#define AF_INET         2
#define AF_UNIX         1

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define SOCK_NONBLOCK   04000
#define SOCK_CLOEXEC    02000000

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_KEEPALIVE    9
#define SO_BROADCAST    6
#define SO_LINGER       13
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_RCVBUF       8
#define SO_SNDBUF       7
#define SO_TIMESTAMP    29
#define SO_PRIORITY     12
#define SO_MARK         36
#define SO_BUSY_POLL    46
#define SO_MAX_PACING_RATE 34
#define SO_NO_CHECK     11

#define SOL_TCP         6
#define SOL_IP          0

#define TCP_NODELAY     1
#define TCP_CORK        3
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6
#define TCP_INFO        11

#define IP_TTL          2
#define IP_MTU          14
#define IP_OPTIONS      1
#define IP_RECVTTL      33
#define IP_RECVDSTADDR 20
#define IP_FREEBIND 21

#define ETH_P_ALL       0x0003

#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IP      0

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

/* Socket state machine */
enum sock_state {
    SOCK_STATE_FREE = 0,
    SOCK_STATE_CREATED,
    SOCK_STATE_BOUND,
    SOCK_STATE_LISTENING,
    SOCK_STATE_CONNECTING,
    SOCK_STATE_CONNECTED,
    SOCK_STATE_CLOSED,
};

/* Maximum open sockets */
#define SOCK_MAX 32

/* Socket address structures */
struct in_addr {
    uint32_t s_addr;
};

struct sockaddr {
    uint16_t sa_family;   /* AF_xxx */
    char     sa_data[14];
};

struct sockaddr_in {
    uint16_t        sin_family;  /* AF_INET */
    uint16_t        sin_port;    /* port in network byte order */
    struct in_addr  sin_addr;    /* internet address */
    char            sin_zero[8];
};

/* Socket I/O (msghdr for sendmsg/recvmsg) */
struct msghdr {
    void         *msg_name;       /* ptr to socket address structure */
    uint32_t      msg_namelen;    /* size of socket address structure */
    struct iovec *msg_iov;        /* scatter/gather array */
    uint32_t      msg_iovlen;     /* number of elements in msg_iov */
    void         *msg_control;    /* ancillary data */
    uint64_t      msg_controllen; /* ancillary data buffer len */
    int           msg_flags;      /* flags on received message */
};

/* tcp_info structure for TCP_INFO getsockopt */
struct tcp_info {
    uint8_t  tcpi_state;
    uint8_t  tcpi_ca_state;
    uint8_t  tcpi_retransmits;
    uint8_t  tcpi_probes;
    uint8_t  tcpi_backoff;
    uint8_t  tcpi_options;
    uint8_t  tcpi_snd_wscale : 4, tcpi_rcv_wscale : 4;
    uint32_t tcpi_rto;
    uint32_t tcpi_ato;
    uint32_t tcpi_snd_mss;
    uint32_t tcpi_rcv_mss;
    uint32_t tcpi_unacked;
    uint32_t tcpi_sacked;
    uint32_t tcpi_lost;
    uint32_t tcpi_retrans;
    uint32_t tcpi_fackets;
    /* Times */
    uint32_t tcpi_last_data_sent;
    uint32_t tcpi_last_ack_sent;
    uint32_t tcpi_last_data_recv;
    uint32_t tcpi_last_ack_recv;
    /* Metrics */
    uint32_t tcpi_pmtu;
    uint32_t tcpi_rcv_ssthresh;
    uint32_t tcpi_rtt;
    uint32_t tcpi_rttvar;
    uint32_t tcpi_snd_ssthresh;
    uint32_t tcpi_snd_cwnd;
    uint32_t tcpi_advmss;
    uint32_t tcpi_reordering;
    uint32_t tcpi_rcv_rtt;
    uint32_t tcpi_rcv_space;
    uint32_t tcpi_total_retrans;
};

/* Per-socket structure */
struct socket {
    int           in_use;
    int           domain;      /* AF_INET, AF_UNIX */
    int           type;        /* SOCK_STREAM, SOCK_DGRAM */
    int           protocol;    /* IPPROTO_TCP, IPPROTO_UDP */
    enum sock_state state;
    /* Addressing */
    uint32_t      local_ip;
    uint16_t      local_port;
    uint32_t      remote_ip;
    uint16_t      remote_port;
    /* TCP connection ID (returned by net stack) */
    int           conn_id;
    /* Listening backlog */
    int           backlog;
    /* Socket options */
    int           reuseaddr;
    int           keepalive;
    int           rcvbuf;
    int           sndbuf;
    int           tcp_nodelay;
    int           tcp_cork;
    int           ip_ttl;
    int           broadcast;
    int           priority;           /* SO_PRIORITY */
    uint32_t      sk_mark;            /* SO_MARK */
    int           busy_poll_usecs;    /* SO_BUSY_POLL */
    uint32_t      max_pacing_rate;    /* SO_MAX_PACING_RATE */
    int           no_check;           /* SO_NO_CHECK (UDP checksum disable) */
    int           ip_recvttl;         /* IP_RECVTTL */
    int           ip_recvdstaddr;     /* IP_RECVDSTADDR */
    /* UDP listener index (for net_udp_listen) */
    int           udp_listener;

    /* UDP connected socket route cache — pre-resolved MAC address
     * to avoid ARP cache lookup on every send when the socket is
     * in connected state (SOCK_STATE_CONNECTED).  Populated during
     * connect(), invalidated on explicit sendto() with a different
     * destination or on close(). */
    uint8_t       cached_dst_mac[6];
    int           cache_valid;
};

/* Socket table operations */
struct socket *sock_get(int fd);
int sock_alloc(void);
void sock_free(int fd);
int sock_fd_from_slot(int slot);

/* Initialize socket subsystem */
void socket_init(void);

/* Poll a socket for readiness.
 * @sockfd  Socket FD (must be a valid socket)
 * @events  Requested events (POLLIN | POLLOUT)
 * @return  Bitmask of POLLIN|POLLOUT|POLLHUP|POLLERR|POLLNVAL
 */
int sock_poll(int sockfd, int events);

#endif
