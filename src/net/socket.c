#include "socket.h"
#include "net.h"
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
    if (domain != AF_INET && domain != AF_UNIX) return -1;
    int slot = sock_alloc();
    if (slot < 0) return -1;

    struct socket *s = &socket_table[slot];
    s->domain = domain;
    s->type = type;
    s->protocol = protocol;

    /* Map SOCK_STREAM → TCP, SOCK_DGRAM → UDP */
    if (protocol == 0) {
        s->protocol = (type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
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
            case SO_KEEPALIVE:
                s->keepalive = *(int*)optval;
                return 0;
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
