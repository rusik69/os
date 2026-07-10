#ifndef SOCKET_H
#define SOCKET_H

#include "types.h"
#include "net.h" /* for net_tcp_* API */
#include "waitqueue.h"

/* Forward declaration for poll support */
struct poll_table;

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
#define AF_INET6       10
#define AF_UNIX         1
#define AF_CAN         29      /* Controller Area Network (SocketCAN) */

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define SOCK_SEQPACKET  5
#define SOCK_NONBLOCK   04000
#define SOCK_CLOEXEC    02000000

#define SOL_SOCKET      1
#define SOL_CAN_BASE    100     /* CAN protocol base level */
#define SOL_CAN_RAW     (SOL_CAN_BASE + CAN_RAW)   /* = 101 */
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

/* UNIX domain socket address */
#define UNIX_PATH_MAX 108
struct sockaddr_un {
    uint16_t sun_family;          /* AF_UNIX */
    char     sun_path[UNIX_PATH_MAX];  /* pathname */
};

/* ── Interface request structure (net/if.h) ──────────────────────────── */

#define IFNAMSIZ 16

/* Linux-compatible struct ifreq for SIOCGIF* ioctls */
struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifr_addr;
        struct sockaddr ifr_dstaddr;
        struct sockaddr ifr_broadaddr;
        struct sockaddr ifr_hwaddr;
        short           ifr_flags;
        int             ifr_ifindex;
        int             ifr_metric;
    };
};

/* ── Message flags for sendmsg/recvmsg ──────────────────────────── */
#define MSG_OOB         0x0001  /* Out-of-band data */
#define MSG_PEEK        0x0002  /* Peek at incoming message */
#define MSG_DONTROUTE   0x0004  /* Bypass routing */
#define MSG_CTRUNC      0x0008  /* Control data truncated */
#define MSG_PROXY       0x0010  /* Wait for full request */
#define MSG_TRUNC       0x0020  /* Data truncated */
#define MSG_DONTWAIT    0x0040  /* Non-blocking operation */
#define MSG_EOR         0x0080  /* End of record */
#define MSG_WAITALL     0x0100  /* Wait for full request/response */
#define MSG_FIN         0x0200  /* FIN (SCTP) */
#define MSG_SYN         0x0400  /* SYN (SCTP) */
#define MSG_CONFIRM     0x0800  /* Confirm path validity */
#define MSG_RST         0x1000  /* RST (SCTP) */
#define MSG_ERRQUEUE    0x2000  /* Fetch from error queue */
#define MSG_NOSIGNAL    0x4000  /* Do not generate SIGPIPE */
#define MSG_MORE        0x8000  /* Sender will send more */
#define MSG_WAITFORONE  0x10000 /* Wait for at least one packet (epoll) */
#define MSG_BATCH       0x40000 /* Batch send */
#define MSG_FASTOPEN    0x20000000 /* TCP Fast Open */
#define MSG_CMSG_CLOEXEC 0x40000000 /* Set close-on-exec on fd received via SCM_RIGHTS */

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

#include "spinlock.h"

/* Per-socket structure */
struct socket {
    spinlock_t    lock;        /* protects this socket entry (concurrent send/recv/close) */
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

    /* UNIX domain socket endpoint index (or -1 if not AF_UNIX) */
    int           unix_ep;

    /* Poll waitqueue — woken when socket state changes (data arrives,
     * connection established, etc.) for poll/select/epoll support. */
    struct wait_queue wq;
};

/* Socket table operations */
struct socket *sock_get(int fd);
void sock_put(struct socket *s);
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
/* Network poll interface (supports poll/select/epoll via poll_table) */
int sock_poll(int sockfd, int events, struct poll_table *pt);

/*
 * sock_wake_by_conn_id — wake socket waitqueues for a TCP conn_id.
 * Called from the TCP stack when data arrives for poll/select/epoll.
 */
void sock_wake_by_conn_id(int conn_id);

/* ── AF_UNIX socket operations ──────────────────────────────────── */
int unix_create(int type);
void unix_destroy(int endpoint_idx);
int unix_bind(int endpoint_idx, const struct sockaddr_un *addr, uint32_t addrlen);
int unix_listen(int endpoint_idx, int backlog);
int unix_accept(int endpoint_idx, int timeout_ms);
int unix_connect(int endpoint_idx, const struct sockaddr_un *addr, uint32_t addrlen);
int unix_send(int endpoint_idx, const void *data, uint32_t len, int nonblock);
int unix_recv(int endpoint_idx, void *data, uint32_t len, int nonblock);
int unix_sendmsg(int endpoint_idx, const struct msghdr *msg, int flags);
int unix_recvmsg(int endpoint_idx, struct msghdr *msg, int flags);
int unix_shutdown(int endpoint_idx, int how);
int unix_poll(int endpoint_idx, int events);
int unix_getsockname(int endpoint_idx, struct sockaddr_un *addr, uint32_t *addrlen);
int unix_getpeername(int endpoint_idx, struct sockaddr_un *addr, uint32_t *addrlen);
int unix_socketpair(int *ep0, int *ep1);
void af_unix_init(void);

#endif
