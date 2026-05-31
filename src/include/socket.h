#ifndef SOCKET_H
#define SOCKET_H

#include "types.h"
#include "net.h" /* for net_tcp_* API */

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
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_TIMESTAMP    29

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
    /* UDP listener index (for net_udp_listen) */
    int           udp_listener;
};

/* Socket table operations */
struct socket *sock_get(int fd);
int sock_alloc(void);
void sock_free(int fd);
int sock_fd_from_slot(int slot);

/* Initialize socket subsystem */
void socket_init(void);

#endif
