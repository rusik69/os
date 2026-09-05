#include "socket.h"

#include "af_packet.h" /* AF_PACKET raw packet sockets (Item 386) */
#include "can.h"
#include "can.h" /* AF_CAN SocketCAN protocol (Item 352) */
#include "errno.h"
#include "export.h"
#include "lsm.h"
#include "module.h" /* request_module() for protocol autoloading */
#include "net.h"
#include "net_internal.h"
#include "netlink.h" /* AF_NETLINK kernel-userspace sockets (Item 384) */
#include "poll.h"
#include "printf.h"
#include "process.h"
#include "scheduler.h"
#include "string.h"
#include "timer.h"
#include "types.h"

/* ══════════════════════════════════════════════════════════════════════
 * Socket API Dispatch
 * ══════════════════════════════════════════════════════════════════════
 *
 * Architecture overview
 * ─────────────────────
 * This file implements the kernel socket subsystem — the glue between
 * the POSIX socket syscall interface and the underlying protocol
 * implementations (TCP, UDP, AF_UNIX, AF_PACKET, AF_NETLINK, AF_CAN).
 *
 *                   ┌──────────────┐
 *                   │  Userspace   │  (socket, bind, connect, send, …)
 *                   └──────┬───────┘
 *                          │ syscall
 *                          ▼
 *                   ┌──────────────┐
 *                   │  socket.c    │  sys_*_impl() dispatch functions
 *                   │  (this file) │   │
 *                   └───┬───┬───┬──┘   ├─ domain == AF_UNIX    → unix_*()
 *                       │   │   │      ├─ domain == AF_PACKET  → packet_*()
 *              ┌────────┘   │   └───┐  ├─ domain == AF_NETLINK → netlink_*()
 *              ▼            ▼       ▼  ├─ domain == AF_CAN     → can_*()
 *         ┌────────┐ ┌────────┐ ┌────┐ └─ (default) AF_INET    → net_tcp_* / net_udp_*
 *         │unix_*()│ │packet*││netlink│
 *         └────────┘ └────────┘ └────┘
 *
 * Socket table (fixed-size)
 * ─────────────────────────
 * Sockets are stored in a fixed-size array `socket_table[SOCK_MAX]`
 * indexed by slot number.  File descriptors are derived as:
 *
 *     fd = slot + 100
 *
 * The +100 offset avoids collision with normal file descriptors
 * (stdin/stdout/stderr and VFS-based open() calls).
 *
 * Locking model
 * ─────────────
 * Two locks protect socket state:
 *
 *   1. socket_lock (global) — serialises sock_alloc() and sock_free()
 *      so no slot is recycled while protocol teardown is in progress.
 *   2. s->lock (per-socket) — acquired by sock_get(), released by
 *      sock_put().  Protects in_use, state, conn_id and other fields
 *      so that concurrent operations on the same socket see a
 *      consistent view.
 *
 * Lock ordering:  socket_lock → s->lock  (never the reverse).
 *
 * Dispatch pattern
 * ────────────────
 * Each sys_*_impl() function follows the same pattern:
 *
 *   1. sock_get(fd) → obtain socket pointer with s->lock held.
 *   2. Validate arguments (bounds, state, null checks).
 *   3. Switch on s->domain to dispatch to the correct handler:
 *         AF_UNIX    → unix_create / unix_bind / unix_sendmsg / …
 *         AF_PACKET  → packet_create / packet_bind / packet_send / …
 *         AF_NETLINK → netlink_create / netlink_bind / netlink_sendmsg / …
 *         AF_CAN     → can_create / can_bind / can_sendmsg / …
 *         (default)  → net_tcp_* / net_udp_* (AF_INET)
 *   4. sock_put(s) to release per-socket lock.
 *   5. Return result to caller.
 *
 * Supported address families
 * ──────────────────────────
 *   AF_UNIX    — Local UNIX domain sockets (filesystem or abstract)
 *   AF_INET    — TCP/IPv4 (SOCK_STREAM) and UDP/IPv4 (SOCK_DGRAM)
 *   AF_INET6   — Not natively supported; triggers request_module("ipv6")
 *                for optional module autoloading.
 *   AF_PACKET  — Raw packet sockets (ETH_P_ALL, etc.)
 *   AF_NETLINK — Kernel-userspace netlink IPC
 *   AF_CAN     — CAN bus (SocketCAN)
 *   domain=0   — Legacy raw socket compat (maps to AF_PACKET)
 *
 * Socket states
 * ─────────────
 *   SOCK_STATE_CREATED     — After sock_alloc()
 *   SOCK_STATE_BOUND       — After successful bind()
 *   SOCK_STATE_LISTENING   — After successful listen()
 *   SOCK_STATE_CONNECTING  — connect() in progress (non-blocking)
 *   SOCK_STATE_CONNECTED   — After successful connect() or accept()
 *   SOCK_STATE_FREE        — Slot released, available for re-use
 *
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Compile-time struct size assertions ────────────────────────────── */
_Static_assert(sizeof(struct socket) >= 64,
               "struct socket must be at least 64 bytes for fixed-size table");
_Static_assert(sizeof(struct sockaddr_in) == 16, "sockaddr_in must be 16 bytes (ABI)");
_Static_assert(sizeof(struct tcp_info) == 104, "tcp_info must be 104 bytes (ABI)");

/* ── Socket table ────────────────────────────────────────────── */
static struct socket socket_table[SOCK_MAX];

/* ── Global socket table lock ───────────────────────────────────
 * Protects sock_alloc (find-and-claim) and sock_free (cleanup-and-release)
 * so that no slot is reused while cleanup is still in progress.
 *
 * Per-socket s->lock (acquired by sock_get / released by sock_put)
 * protects the in_use / state / conn_id fields so that operations
 * (send, recv, bind, …) see a consistent view and sock_free cannot
 * destroy resources while a concurrent operation is in flight.
 *
 * Lock ordering:  socket_lock → s->lock
 * No code path ever acquires s->lock → socket_lock in reverse.
 */
static spinlock_t socket_lock;

/**
 * socket_init - Initialize the socket subsystem
 *
 * Zeroes the socket table, initialises the global socket lock, and registers each address family
 * (AF_UNIX, AF_PACKET, AF_NETLINK, AF_CAN) by calling its init hook. Called once during kernel
 * bring-up.
 */
void socket_init(void) {
    memset(socket_table, 0, sizeof(socket_table));
    spinlock_init(&socket_lock);
    af_unix_init();
    af_packet_init();
    af_netlink_init();
    can_init();
}

/* Convert slot to fd number (fd = slot + 100 to avoid conflict with normal fds) */
/**
 * sock_fd_from_slot - Convert a socket table slot index to a file descriptor
 * @slot: Index into the socket table
 *
 * Sockets are exposed to userspace via fd numbers offset by +100 so they never collide with
 * ordinary file descriptors.
 */
int sock_fd_from_slot(int slot) {
    return slot + 100;
}

/* Get socket struct from fd.
 *
 * Returns with s->lock held.  Caller MUST call sock_put(s) when done
 * to release the lock.  This prevents the socket from being freed
 * while in use (see sock_free which checks in_use under the same lock).
 */
struct socket *sock_get(int fd) {
    int slot = fd - 100;
    if (slot < 0 || slot >= SOCK_MAX)
        return NULL;
    struct socket *s = &socket_table[slot];
    spinlock_acquire(&s->lock);
    if (!s->in_use) {
        spinlock_release(&s->lock);
        return NULL;
    }
    return s; /* lock held — caller must sock_put */
}

/* Release a socket obtained from sock_get. */
/**
 * sock_put - Release a socket acquired via sock_get
 * @s: Socket previously returned by sock_get
 *
 * Releases the per-socket spinlock taken by sock_get so a concurrent sock_free or operation may
 * proceed.
 */
void sock_put(struct socket *s) {
    spinlock_release(&s->lock);
}

/* Allocate a socket slot */
/**
 * sock_alloc - Allocate a free socket table slot
 *
 * Finds the first unused slot under the global socket_lock, initialises its fields to a
 * freshly-created socket, and returns the slot index. Returns -ENOMEM if the table is full.
 */
int sock_alloc(void) {
    spinlock_acquire(&socket_lock);
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!socket_table[i].in_use) {
            memset(&socket_table[i], 0, sizeof(struct socket));
            spinlock_init(&socket_table[i].lock);
            socket_table[i].in_use = 1;
            socket_table[i].state = SOCK_STATE_CREATED;
            socket_table[i].conn_id = -1;
            socket_table[i].udp_listener = -1;
            wait_queue_init(&socket_table[i].wq);
            spinlock_release(&socket_lock);
            return i;
        }
    }
    spinlock_release(&socket_lock);
    return -ENOMEM;
}

/* Free a socket.
 *
 * Under socket_lock (serialises with sock_alloc) and the per-socket
 * lock (serialises with sock_get / sock_put), marks the slot as free
 * and tears down all protocol resources.
 */
/**
 * sock_free - Free a socket and tear down all protocol resources
 * @fd: Socket file descriptor to release
 *
 * Under the global socket_lock and the per-socket lock, marks the slot as free and destroys any
 * AF_UNIX endpoint, AF_PACKET, AF_NETLINK, AF_CAN socket, TCP connection or UDP listener associated
 * with it.
 */
void sock_free(int fd) {
    int slot = fd - 100;
    if (slot < 0 || slot >= SOCK_MAX)
        return;
    struct socket *s = &socket_table[slot];

    spinlock_acquire(&socket_lock);

    /* Under per-socket lock, mark destroyed so sock_get returns NULL */
    spinlock_acquire(&s->lock);
    if (!s->in_use) {
        spinlock_release(&s->lock);
        spinlock_release(&socket_lock);
        return;
    }
    s->in_use = 0;
    s->state = SOCK_STATE_FREE;

    /* Snapshot fields needed for teardown while locks are held */
    int domain = s->domain;
    int type = s->type;
    int conn_id = s->conn_id;
    int unix_ep = s->unix_ep;
    int udp_listener = s->udp_listener;
    uint16_t local_port = s->local_port;

    /* Clear pointers so a racing sock_get sees safe default values */
    s->conn_id = -1;
    s->unix_ep = -1;
    s->udp_listener = -1;

    spinlock_release(&s->lock);
    /* socket_lock still held — prevents sock_alloc from recycling
     * this slot before protocol-level teardown completes */

    /* Destroy AF_UNIX endpoint if present */
    if (domain == AF_UNIX && unix_ep >= 0)
        unix_destroy(unix_ep);
    /* Destroy AF_PACKET socket if present */
    if (domain == AF_PACKET || (domain == 0 && type == SOCK_RAW))
        packet_close(fd);
    /* Destroy AF_NETLINK socket if present */
    if (domain == AF_NETLINK)
        netlink_close(fd);
    /* Destroy AF_CAN socket if present */
    if (domain == AF_CAN)
        can_close(fd);
    /* Close TCP connection if present */
    if (conn_id >= 0)
        net_tcp_close(conn_id);
    /* Close UDP listener if present */
    if (type == SOCK_DGRAM && udp_listener >= 0)
        net_udp_unlisten(local_port);

    spinlock_release(&socket_lock);
}

/* ── Socket syscall implementations ──────────────────────────── */

/**
 * sys_socket_impl - Create a new socket
 * @domain: Address family (AF_INET, AF_UNIX, AF_PACKET, ...)
 * @type: Socket type (SOCK_STREAM, SOCK_DGRAM, SOCK_RAW, ...)
 * @protocol: Protocol (IPPROTO_TCP, IPPROTO_UDP, or 0)
 *
 * Implements the socket(2) syscall. Consults the LSM socket_create hook, attempts on-demand
 * autoloading of protocol modules for unsupported families, then allocates a socket slot and
 * returns its fd.
 */
int sys_socket_impl(int domain, int type, int protocol) {
    /* LSM socket_create hook: security modules may deny creation of
     * particular socket families/types/protocols up front. */
    {
        int lsm_ret = lsm_socket_create(domain, type, protocol);
        if (lsm_ret < 0)
            return lsm_ret;
    }
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
        if (domain != 0 && domain != 17 && domain != AF_NETLINK && domain != AF_CAN)
            return -EAFNOSUPPORT;
    }

    /* Validate socket type for AF_NETLINK — Linux only allows
     * SOCK_RAW (direct message access) or SOCK_DGRAM for netlink. */
    if (domain == AF_NETLINK && type != SOCK_RAW && type != SOCK_DGRAM)
        return -EPROTONOSUPPORT;

    int slot = sock_alloc();
    if (slot < 0)
        return slot; /* -ENOMEM */

    struct socket *s = &socket_table[slot];

    /* Extract SOCK_NONBLOCK and SOCK_CLOEXEC flags from type */
    if (type & SOCK_NONBLOCK) {
        s->nonblock = 1;
        type &= ~SOCK_NONBLOCK;
    }
    /* SOCK_CLOEXEC handling would go here if we had a close-on-exec mechanism
     * for socket FDs; for now we just strip the flag. */
    type &= ~SOCK_CLOEXEC;

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
        if (ep < 0) {
            sock_free(sock_fd_from_slot(slot));
            return -EINVAL;
        }
        s->unix_ep = ep;
    }

    /* For AF_PACKET: create a raw packet socket endpoint */
    if ((domain == AF_PACKET || domain == 0) && type == SOCK_RAW) {
        int ret = packet_create(sock_fd_from_slot(slot), type, (uint16_t)s->protocol);
        if (ret < 0) {
            sock_free(sock_fd_from_slot(slot));
            return -EINVAL;
        }
    }

    /* For AF_NETLINK: create a netlink socket endpoint */
    if (domain == AF_NETLINK) {
        int proto = (int)protocol;
        if (proto < 0)
            proto = NETLINK_GENERIC; /* Default protocol */
        int ret = netlink_create(sock_fd_from_slot(slot), proto);
        if (ret < 0) {
            sock_free(sock_fd_from_slot(slot));
            return -EINVAL;
        }
    }

    /* For AF_CAN: create a CAN bus socket endpoint (Item 352) */
    if (domain == AF_CAN) {
        int can_proto = (int)protocol;
        if (can_proto <= 0)
            can_proto = CAN_RAW; /* Default to RAW */
        int ret = can_create(can_proto);
        if (ret < 0) {
            sock_free(sock_fd_from_slot(slot));
            return -EINVAL;
        }
    }

    return sock_fd_from_slot(slot);
}

/**
 * sys_bind_impl - Bind a socket to a local address
 * @sockfd: Socket file descriptor
 * @addr: Local sockaddr_in to bind
 * @addrlen: Length of @addr
 *
 * Implements the bind(2) syscall. Records the local IP and port on the socket; for stream/TCP
 * sockets it retries port allocation on EADDRINUSE when SO_REUSEADDR is set, and for UDP it
 * registers a listener.
 */
int sys_bind_impl(int sockfd, const struct sockaddr_in *addr, int addrlen) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return -EBADF;

    int ret = 0;

    /* Validate minimum address length (at least the family field) */
    if (addrlen < (int)sizeof(uint16_t)) {
        ret = -EINVAL;
        goto out;
    }

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX) {
        if (addrlen < (int)sizeof(struct sockaddr_un)) {
            ret = -EINVAL;
            goto out;
        }
        if (s->unix_ep < 0) {
            ret = -EINVAL;
            goto out;
        }
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        ret = unix_bind(s->unix_ep, un, (uint32_t)addrlen);
        if (ret == 0)
            s->state = SOCK_STATE_BOUND;
        goto out;
    }

    /* AF_PACKET: dispatch to raw packet handler */
    if (s->domain == AF_PACKET || (s->domain == 0 && s->type == SOCK_RAW)) {
        if (addrlen < (int)sizeof(struct sockaddr_ll)) {
            ret = -EINVAL;
            goto out;
        }
        /* sockaddr_ll structure cast from addr */
        const struct sockaddr_ll *sll = (const struct sockaddr_ll *)addr;
        /* Bind to interface index (0 = any interface) */
        ret = packet_bind(sockfd, (int)sll->sll_ifindex);
        if (ret == 0)
            s->state = SOCK_STATE_BOUND;
        goto out;
    }

    /* AF_NETLINK: dispatch to netlink handler */
    if (s->domain == AF_NETLINK) {
        if (addrlen < (int)sizeof(struct sockaddr_nl)) {
            ret = -EINVAL;
            goto out;
        }
        const struct sockaddr_nl *nl_addr = (const struct sockaddr_nl *)addr;
        ret = netlink_bind(sockfd, nl_addr);
        if (ret == 0)
            s->state = SOCK_STATE_BOUND;
        goto out;
    }

    /* AF_CAN: dispatch to CAN bus handler */
    if (s->domain == AF_CAN) {
        if (addrlen < (int)sizeof(struct sockaddr_can)) {
            ret = -EINVAL;
            goto out;
        }
        const struct sockaddr_can *can_addr = (const struct sockaddr_can *)addr;
        ret = can_bind(sockfd, can_addr);
        if (ret == 0)
            s->state = SOCK_STATE_BOUND;
        goto out;
    }

    /* AF_INET (or default) — sockaddr_in */
    if (addrlen < (int)sizeof(struct sockaddr_in)) {
        ret = -EINVAL;
        goto out;
    }
    s->local_ip = addr->sin_addr.s_addr;
    s->local_port = ntohs(addr->sin_port);

    /* Check for port conflict — reject if port is already in use
     * unless SO_REUSEADDR is set on this socket.  This implements
     * standard BSD SO_REUSEADDR semantics: a socket with reuseaddr
     * may bind to a port that is already bound by another socket
     * (e.g. a previous instance in TIME_WAIT). */
    if (s->type == SOCK_STREAM) {
        if (net_tcp_port_in_use(s->local_port) && !s->reuseaddr) {
            ret = -EADDRINUSE;
            goto out;
        }
    } else if (s->type == SOCK_DGRAM) {
        if (net_udp_port_in_use(s->local_port) && !s->reuseaddr) {
            ret = -EADDRINUSE;
            goto out;
        }
    }

    s->state = SOCK_STATE_BOUND;

    /* For UDP, bind the port */
    if (s->type == SOCK_DGRAM) {
        s->udp_listener = net_udp_listen(s->local_port);
        if (s->udp_listener < 0) {
            ret = -EADDRINUSE;
            goto out;
        }
    }

out:
    sock_put(s);
    return ret;
}

/**
 * sys_listen_impl - Mark a socket as listening for connections
 * @sockfd: Socket file descriptor
 * @backlog: Maximum length of the pending-connections queue
 *
 * Implements the listen(2) syscall. Transitions the socket to the listening state and starts
 * accepting inbound connections for the bound port.
 */
int sys_listen_impl(int sockfd, int backlog) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return -EBADF;

    if (s->state != SOCK_STATE_BOUND) {
        sock_put(s);
        return -EINVAL;
    }
    if (s->type != SOCK_STREAM) {
        sock_put(s);
        return -EOPNOTSUPP;
    }

    int ret = 0;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX) {
        if (s->unix_ep < 0) {
            sock_put(s);
            return -EINVAL;
        }
        ret = unix_listen(s->unix_ep, backlog);
        if (ret == 0)
            s->state = SOCK_STATE_LISTENING;
        sock_put(s);
        return ret;
    }

    s->backlog = backlog;
    s->state = SOCK_STATE_LISTENING;

    /* Register TCP listener */
    net_tcp_listen(s->local_port, NULL, NULL, NULL);

    sock_put(s);
    return 0;
}

/**
 * sys_accept_impl - Accept an incoming connection on a listening socket
 * @sockfd: Listening socket file descriptor
 * @addr: Optional caller buffer to store the peer address
 * @addrlen: In/out length of @addr
 *
 * Implements the accept(2) syscall. Waits for a pending connection, allocates a new connected
 * socket, and returns its fd; fills @addr with the peer address if requested.
 */
int sys_accept_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return -EBADF;

    if (s->state != SOCK_STATE_LISTENING) {
        sock_put(s);
        return -EINVAL;
    }

    int domain = s->domain;
    int type = s->type;
    int local_port = s->local_port;
    int unix_ep = s->unix_ep;
    sock_put(s);
    s = NULL;

    /* AF_UNIX: dispatch to local socket handler */
    if (domain == AF_UNIX) {
        if (unix_ep < 0)
            return -EINVAL;
        int client_ep = unix_accept(unix_ep, 0);
        if (client_ep < 0)
            return -EINVAL;

        /* Allocate a new socket for the accepted connection */
        int new_slot = sock_alloc();
        if (new_slot < 0) {
            unix_destroy(client_ep);
            return -ENOMEM;
        }

        struct socket *ns = &socket_table[new_slot];
        ns->domain = AF_UNIX;
        ns->type = type;
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
    int conn_id = net_tcp_accept(local_port, 10000); /* 100 second timeout */
    if (conn_id < 0)
        return -ETIMEDOUT;

    /* Allocate a new socket for the accepted connection */
    int new_slot = sock_alloc();
    if (new_slot < 0) {
        net_tcp_close(conn_id);
        return -ENOMEM;
    }

    struct socket *ns = &socket_table[new_slot];
    ns->domain = domain;
    ns->type = type;
    ns->protocol = 0;
    ns->state = SOCK_STATE_CONNECTED;
    ns->conn_id = conn_id;
    ns->local_port = (uint16_t)local_port;
    /* Copy remote address from TCP connection — sock_alloc zeroes the
     * socket, so ns->remote_ip/remote_port are still 0.0.0.0:0. */
    ns->remote_ip = tcp_conns[conn_id].remote_ip;
    ns->remote_port = tcp_conns[conn_id].remote_port;

    /* Fill in peer address if requested */
    if (addr && addrlen) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons(ns->remote_port);
        addr->sin_addr.s_addr = ns->remote_ip;
        *addrlen = sizeof(struct sockaddr_in);
    }

    return sock_fd_from_slot(new_slot);
}

/**
 * sys_connect_impl - Initiate a connection to a remote address
 * @sockfd: Socket file descriptor
 * @addr: Remote sockaddr_in to connect to
 *
 * Implements the connect(2) syscall. For stream sockets performs a TCP connect over the network
 * stack; for datagram sockets records the destination and caches the resolved MAC for fast send.
 */
int sys_connect_impl(int sockfd, const struct sockaddr_in *addr) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return -EBADF;

    /* LSM socket_connect hook: security modules may restrict which
     * sockets a subject is allowed to connect. */
    {
        int lsm_ret = lsm_socket_connect(s->domain, s->type, 0);
        if (lsm_ret < 0)
            return lsm_ret;
    }

    int ret = 0;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX) {
        if (s->unix_ep < 0) {
            ret = -EINVAL;
            goto out;
        }
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        ret = unix_connect(s->unix_ep, un, sizeof(struct sockaddr_un));
        if (ret == 0)
            s->state = SOCK_STATE_CONNECTED;
        goto out;
    }

    s->remote_ip = addr->sin_addr.s_addr;
    s->remote_port = ntohs(addr->sin_port);

    if (s->type == SOCK_STREAM) {
        int conn_id = net_tcp_connect(s->remote_ip, s->remote_port);
        if (conn_id < 0) {
            ret = conn_id;
            goto out;
        }
        s->conn_id = conn_id;
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

out:
    sock_put(s);
    return ret;
}

/* Validate socket option level against socket domain/type.
 * Prevents dispatch of protocol-specific options to wrong socket types.
 * Returns 0 on success, -ENOPROTOOPT if the level is inappropriate. */
static int sock_validate_level(struct socket *s, int level) {
    /* SOL_SOCKET is valid for all sockets */
    if (level == SOL_SOCKET)
        return 0;

    /* SOL_TCP requires a TCP socket (AF_INET/AF_INET6 + SOCK_STREAM) */
    if (level == SOL_TCP) {
        if (s->domain != AF_INET && s->domain != AF_INET6)
            return -ENOPROTOOPT;
        if (s->type != SOCK_STREAM)
            return -ENOPROTOOPT;
        return 0;
    }

    /* SOL_IP requires an IP socket (AF_INET/AF_INET6) */
    if (level == SOL_IP) {
        if (s->domain != AF_INET && s->domain != AF_INET6)
            return -ENOPROTOOPT;
        return 0;
    }

    /* SOL_CAN_RAW / SOL_CAN_BASE require a CAN socket */
    if (level == SOL_CAN_RAW || level == SOL_CAN_BASE) {
        if (s->domain != AF_CAN)
            return -ENOPROTOOPT;
        return 0;
    }

    /* Any other level is unknown */
    return -ENOPROTOOPT;
}

/**
 * sys_setsockopt_impl - Set a socket option
 * @sockfd: Socket file descriptor
 * @level: Option level (SOL_SOCKET, SOL_TCP, SOL_IP)
 * @optname: Option name (SO_REUSEADDR, TCP_NODELAY, IP_TTL, ...)
 * @optval: Pointer to the option value
 * @optlen: Length of the option value
 *
 * Implements the setsockopt(2) syscall. Validates the option level, then applies socket, TCP, or IP
 * options to the socket structure.
 */
int sys_setsockopt_impl(int sockfd, int level, int optname, const void *optval, uint32_t optlen) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return -EBADF;

    /* Validate socket option level before dispatch */
    {
        int ret = sock_validate_level(s, level);
        if (ret) {
            sock_put(s);
            return ret;
        }
    }

    /* Validate optlen — must be at least sizeof(int) for integer options */
    if (!optval || optlen < sizeof(int)) {
        sock_put(s);
        return -EINVAL;
    }

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
            s->reuseaddr = *(const int *)optval;
            sock_put(s);
            return 0;
        case SO_KEEPALIVE: {
            s->keepalive = *(const int *)optval;
            if (s->conn_id >= 0)
                net_tcp_set_keepalive(s->conn_id, s->keepalive);
            sock_put(s);
            return 0;
        }
        case SO_RCVBUF:
            s->rcvbuf = *(const int *)optval;
            if (s->rcvbuf < 256)
                s->rcvbuf = 256;
            sock_put(s);
            return 0;
        case SO_SNDBUF:
            s->sndbuf = *(const int *)optval;
            if (s->sndbuf < 256)
                s->sndbuf = 256;
            sock_put(s);
            return 0;
        case SO_BROADCAST:
            s->broadcast = *(const int *)optval;
            sock_put(s);
            return 0;
        case SO_PRIORITY:
            s->priority = *(const int *)optval;
            sock_put(s);
            return 0;
        case SO_MARK:
            s->sk_mark = *(const uint32_t *)optval;
            sock_put(s);
            return 0;
        case SO_BUSY_POLL:
            s->busy_poll_usecs = *(const int *)optval;
            sock_put(s);
            return 0;
        case SO_MAX_PACING_RATE:
            s->max_pacing_rate = *(const uint32_t *)optval;
            sock_put(s);
            return 0;
        case SO_NO_CHECK:
            s->no_check = *(const int *)optval;
            sock_put(s);
            return 0;
        case SO_RCVTIMEO: {
            const struct timeval *tv = (const struct timeval *)optval;
            s->busy_poll_usecs = (int)(tv->tv_sec * 1000000 + tv->tv_usec);
            sock_put(s);
            return 0;
        }
        case SO_SNDTIMEO: {
            const struct timeval *tv = (const struct timeval *)optval;
            s->max_pacing_rate = (uint32_t)(tv->tv_sec * 1000000 + tv->tv_usec);
            sock_put(s);
            return 0;
        }
        default:
            break;
        }
    } else if (level == SOL_TCP) {
        switch (optname) {
        case TCP_NODELAY: {
            int val = *(const int *)optval;
            s->tcp_nodelay = val;
            if (s->conn_id >= 0)
                net_tcp_set_nodelay(s->conn_id, val);
            sock_put(s);
            return 0;
        }
        case TCP_CORK: {
            int val = *(const int *)optval;
            s->tcp_cork = val;
            if (s->conn_id >= 0)
                net_tcp_set_cork(s->conn_id, val);
            sock_put(s);
            return 0;
        }
        case TCP_KEEPIDLE:
        case TCP_KEEPINTVL:
        case TCP_KEEPCNT:
            /* Keepalive tuning — store for later use if needed */
            sock_put(s);
            return 0;
        default:
            break;
        }
    } else if (level == SOL_IP) {
        switch (optname) {
        case IP_TTL: {
            int val = *(const int *)optval;
            s->ip_ttl = val;
            sock_put(s);
            return 0;
        }
        case IP_RECVTTL: {
            s->ip_recvttl = *(const int *)optval;
            sock_put(s);
            return 0;
        }
        case IP_RECVDSTADDR: {
            s->ip_recvdstaddr = *(const int *)optval;
            sock_put(s);
            return 0;
        }
        case IP_FREEBIND: {
            s->broadcast = *(const int *)optval;
            sock_put(s);
            return 0;
        }
        default:
            break;
        }
    } else if (level == SOL_CAN_RAW || level == SOL_CAN_BASE) {
        /* AF_CAN: socket options */
        if (s->domain == AF_CAN) {
            int ret = can_setsockopt(sockfd, level, optname, optval, optlen);
            sock_put(s);
            return ret;
        }
    } else {
        /* Unknown/unrecognized option level — reject */
        sock_put(s);
        return -ENOPROTOOPT;
    }
    sock_put(s);
    return 0;
}

/**
 * sys_getsockopt_impl - Get a socket option
 * @sockfd: Socket file descriptor
 * @level: Option level (SOL_SOCKET, SOL_TCP, SOL_IP)
 * @optname: Option name to query
 * @optval: Buffer to receive the option value
 * @optlen: In/out length of @optval
 *
 * Implements the getsockopt(2) syscall. Returns the current value of the requested socket, TCP, or
 * IP option, including SO_TYPE, SO_ERROR, and TCP_INFO.
 */
int sys_getsockopt_impl(int sockfd, int level, int optname, void *optval, uint32_t *optlen) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return -EBADF;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_TYPE: {
            int val = s->type;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case SO_ERROR: {
            int val = 0;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case SO_RCVBUF: {
            int val = s->rcvbuf ? s->rcvbuf : 65536;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case SO_SNDBUF: {
            int val = s->sndbuf ? s->sndbuf : 65536;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case SO_KEEPALIVE: {
            int val = s->keepalive;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case SO_REUSEADDR: {
            int val = s->reuseaddr;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case SO_PRIORITY: {
            int val = s->priority;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case SO_MARK: {
            uint32_t val = s->sk_mark;
            if (*optlen > sizeof(uint32_t))
                *optlen = sizeof(uint32_t);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case SO_BUSY_POLL: {
            int val = s->busy_poll_usecs;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case SO_MAX_PACING_RATE: {
            uint32_t val = s->max_pacing_rate;
            if (*optlen > sizeof(uint32_t))
                *optlen = sizeof(uint32_t);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case SO_NO_CHECK: {
            int val = s->no_check;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        default:
            break;
        }
    } else if (level == SOL_TCP) {
        switch (optname) {
        case TCP_NODELAY: {
            int val = (s->conn_id >= 0) ? net_tcp_get_nodelay(s->conn_id) : s->tcp_nodelay;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case TCP_CORK: {
            int val = (s->conn_id >= 0) ? net_tcp_get_cork(s->conn_id) : s->tcp_cork;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case TCP_INFO: {
            struct tcp_info info;
            memset(&info, 0, sizeof(info));
            if (s->conn_id >= 0) {
                net_tcp_get_tcpinfo(s->conn_id, &info);
                info.tcpi_ca_state = 0;
                info.tcpi_probes = 0;
                info.tcpi_backoff = 0;
                info.tcpi_options = 0;
                info.tcpi_snd_wscale = 0;
                info.tcpi_rcv_wscale = 0;
                info.tcpi_snd_mss = 1460;
                info.tcpi_rcv_mss = 1460;
                info.tcpi_lost = 0;
                info.tcpi_pmtu = 1500;
                info.tcpi_reordering = 3;
            } else {
                info.tcpi_snd_cwnd = 1;
                info.tcpi_rtt = 0;
            }
            uint32_t copylen = sizeof(info);
            if (*optlen < copylen)
                copylen = *optlen;
            memcpy(optval, &info, copylen);
            *optlen = copylen;
            sock_put(s);
            return 0;
        }
        default:
            break;
        }
    } else if (level == SOL_IP) {
        switch (optname) {
        case IP_TTL: {
            int val = s->ip_ttl ? s->ip_ttl : 64;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case IP_MTU: {
            int val = 1500;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case IP_OPTIONS: {
            /* Return empty options */
            uint8_t empty = 0;
            if (*optlen > sizeof(uint8_t))
                *optlen = sizeof(uint8_t);
            memcpy(optval, &empty, *optlen);
            sock_put(s);
            return 0;
        }
        case IP_RECVTTL: {
            int val = s->ip_recvttl;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        case IP_RECVDSTADDR: {
            int val = s->ip_recvdstaddr;
            if (*optlen > sizeof(int))
                *optlen = sizeof(int);
            memcpy(optval, &val, *optlen);
            sock_put(s);
            return 0;
        }
        default:
            break;
        }
    }
    sock_put(s);
    return -ENOPROTOOPT;
}

/**
 * sys_sendmsg_impl - Send data on a socket
 * @sockfd: Socket file descriptor
 * @msg: msghdr describing destination and iovec scatter/gather array
 * @flags: Send flags (MSG_DONTWAIT, MSG_NOSIGNAL, ...)
 *
 * Implements the sendmsg(2) syscall. Dispatches to the AF_UNIX, AF_PACKET, AF_NETLINK, AF_CAN or
 * net-based send path according to the socket domain and type, copying the user iovec into a kernel
 * buffer.
 */
int sys_sendmsg_impl(int sockfd, const struct msghdr *msg, int flags) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return -EBADF;

    /* Determine non-blocking mode: per-call MSG_DONTWAIT or socket nonblock flag */
    int nonblock = (flags & MSG_DONTWAIT) || s->nonblock;
    /* Set MSG_DONTWAIT in flags so underlying protocol handlers see it */
    int send_flags = flags;
    if (nonblock)
        send_flags |= MSG_DONTWAIT;

    /* For now, just write the first iovec entry */
    if (msg->msg_iovlen < 1 || !msg->msg_iov) {
        sock_put(s);
        return -EINVAL;
    }

    /* Reject iovec count exceeding IOV_MAX (1024) per POSIX */
    if (msg->msg_iovlen > 1024) {
        sock_put(s);
        return -EINVAL;
    }

    /* Validate msg_controllen to prevent pointer arithmetic overflow in
     * CMSG_FIRSTHDR / CMSG_NXTHDR.  If msg_controllen is large enough that
     * (unsigned char *)msg->msg_control + msg->msg_controllen wraps around,
     * the CMSG_NXTHDR end-of-buffer check is defeated and the ancillary-data
     * walker will read past the supplied buffer. */
    if (msg->msg_control && msg->msg_controllen > 0) {
        uintptr_t ctrl_end = (uintptr_t)msg->msg_control + msg->msg_controllen;
        if (ctrl_end <= (uintptr_t)msg->msg_control) {
            sock_put(s);
            return -EINVAL;
        }
    }

    /* AF_NETLINK: use msghdr-aware sendmsg that flattens all iovecs
     * into one contiguous netlink message. */
    if (s->domain == AF_NETLINK) {
        int ret;
        if (!netlink_is_valid_fd(sockfd))
            ret = -EINVAL;
        else
            ret = netlink_sendmsg(sockfd, msg, flags);
        sock_put(s);
        return ret;
    }

    uint64_t total = 0;
    int error = 0;
    for (uint32_t i = 0; i < msg->msg_iovlen; i++) {
        const void *data = msg->msg_iov[i].iov_base;
        uint64_t len = msg->msg_iov[i].iov_len;
        if (len == 0)
            continue;

        /* AF_UNIX: dispatch to local socket handler using sendmsg */
        if (s->domain == AF_UNIX && s->unix_ep >= 0) {
            int sent = unix_sendmsg(s->unix_ep, msg, send_flags);
            sock_put(s);
            if (sent < 0)
                return sent;
            return sent;
        } else if ((s->domain == AF_PACKET || (s->domain == 0 && s->type == SOCK_RAW)) &&
                   packet_is_valid_fd(sockfd)) {
            /* AF_PACKET raw packet send */
            int sent = packet_send(sockfd, data, (int)(len > 2048 ? 2048 : len));
            if (sent < 0) {
                error = total > 0 ? (int)total : -EIO;
                goto out;
            }
            total += (uint64_t)sent;
        } else if (s->domain == AF_CAN) {
            /* AF_CAN: send CAN frame */
            if (len < sizeof(struct can_frame)) {
                error = -EINVAL;
                goto out;
            }
            int sent = can_send(sockfd, (const struct can_frame *)data);
            if (sent < 0) {
                error = total > 0 ? (int)total : -EIO;
                goto out;
            }
            total += (uint64_t)sent;
        } else if (s->type == SOCK_STREAM && s->conn_id >= 0) {
            int sent = net_tcp_send(s->conn_id, data, (uint16_t)(len > 65535 ? 65535 : len));
            if (sent < 0) {
                error = total > 0 ? (int)total : -EIO;
                goto out;
            }
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
                net_udp_send_cached(s->cached_dst_mac, dst_ip, s->local_port, dst_port, data,
                                    (uint16_t)udp_len);
            } else {
                net_udp_send(dst_ip, s->local_port, dst_port, data, (uint16_t)udp_len);
            }
            total += udp_len;
        }
    }
    /* Clamp total to INT32_MAX to avoid signed overflow on return.
     * sendmsg(2) returns ssize_t; this implementation returns int, so
     * values above INT32_MAX cannot be represented correctly. */
    if (total > 0x7FFFFFFFULL)
        total = 0x7FFFFFFFULL;
    error = (int)total;

out:
    sock_put(s);
    return error;
}

/**
 * sys_recvmsg_impl - Receive data on a socket
 * @sockfd: Socket file descriptor
 * @msg: msghdr describing destination buffers and source-address storage
 * @flags: Receive flags (MSG_WAITALL, MSG_DONTWAIT, ...)
 *
 * Implements the recvmsg(2) syscall. Dispatches to the protocol-specific receive path, copies
 * received data into the caller iov, and reports the source address and message flags.
 */
int sys_recvmsg_impl(int sockfd, struct msghdr *msg, int flags) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return -EBADF;

    /* Determine non-blocking mode: per-call MSG_DONTWAIT or socket nonblock flag */
    int nonblock = (flags & MSG_DONTWAIT) || s->nonblock;
    int recv_flags = flags;
    if (nonblock)
        recv_flags |= MSG_DONTWAIT;

    if (msg->msg_iovlen < 1 || !msg->msg_iov) {
        sock_put(s);
        return -EINVAL;
    }

    /* Reject iovec count exceeding IOV_MAX (1024) per POSIX */
    if (msg->msg_iovlen > 1024) {
        sock_put(s);
        return -EINVAL;
    }

    /* Receive into the first iovec buffer */
    void *buf = msg->msg_iov[0].iov_base;
    uint64_t bufsize = msg->msg_iov[0].iov_len;

    /* AF_UNIX: dispatch to local socket handler using recvmsg */
    if (s->domain == AF_UNIX && s->unix_ep >= 0) {
        int n = unix_recvmsg(s->unix_ep, msg, recv_flags);
        sock_put(s);
        if (n <= 0)
            return -EINVAL;
        return n;
    }

    /* AF_PACKET: raw packet receive */
    if ((s->domain == AF_PACKET || (s->domain == 0 && s->type == SOCK_RAW)) &&
        packet_is_valid_fd(sockfd)) {
        uint64_t ifindex = 0;
        int n = packet_recv(sockfd, buf, (int)(bufsize > 2048 ? 2048 : bufsize), &ifindex);
        sock_put(s);
        if (n < 0)
            return n; /* propagate -EAGAIN, -EIO, etc. */
        if (msg->msg_name && n >= 0) {
            struct sockaddr_ll *sll = (struct sockaddr_ll *)msg->msg_name;
            memset(sll, 0, sizeof(struct sockaddr_ll));
            sll->sll_family = AF_PACKET;
            sll->sll_ifindex = (int)ifindex;
            sll->sll_halen = 6; /* Ethernet */
            msg->msg_namelen = sizeof(struct sockaddr_ll);
        }
        return n;
    }

    /* AF_NETLINK: use msghdr-aware recvmsg */
    if (s->domain == AF_NETLINK) {
        int ret;
        if (!netlink_is_valid_fd(sockfd))
            ret = -EINVAL;
        else
            ret = netlink_recvmsg(sockfd, msg, flags);
        sock_put(s);
        return ret;
    }

    /* AF_CAN: CAN frame receive */
    if (s->domain == AF_CAN) {
        if (bufsize < sizeof(struct can_frame)) {
            sock_put(s);
            return -EINVAL;
        }
        int n = can_recv(sockfd, (struct can_frame *)buf);
        sock_put(s);
        if (n < 0)
            return -EINVAL;
        if (msg->msg_name && n >= 0) {
            struct sockaddr_can *scan = (struct sockaddr_can *)msg->msg_name;
            memset(scan, 0, sizeof(struct sockaddr_can));
            scan->can_family = AF_CAN;
            msg->msg_namelen = sizeof(struct sockaddr_can);
        }
        return n;
    }

    int n = -EINVAL;
    if (s->type == SOCK_STREAM && s->conn_id >= 0) {
        int timeout = nonblock ? 1 : 10;
        n = net_tcp_recv(s->conn_id, buf, (uint16_t)(bufsize > 65535 ? 65535 : bufsize), timeout);
        sock_put(s);
        if (n <= 0)
            return nonblock ? -EAGAIN : -EINVAL;
        return n;
    } else if (s->type == SOCK_DGRAM && s->udp_listener >= 0) {
        uint32_t src_ip;
        uint16_t src_port;
        int timeout = nonblock ? 1 : 10;
        n = net_udp_recv((uint16_t)s->local_port, buf, (uint16_t)(bufsize > 1500 ? 1500 : bufsize),
                         &src_ip, &src_port, timeout);
        sock_put(s);
        if (n <= 0)
            return nonblock ? -EAGAIN : -EINVAL;
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
    sock_put(s);
    return n;
}

/**
 * sys_getsockname_impl - Retrieve the local bound address of a socket
 * @sockfd: Socket file descriptor
 * @addr: Buffer to receive the local sockaddr_in
 * @addrlen: In/out length of @addr
 *
 * Implements the getsockname(2) syscall for AF_INET sockets.
 */
int sys_getsockname_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return -EBADF;

    int ret = 0;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX && s->unix_ep >= 0) {
        ret = unix_getsockname(s->unix_ep, (struct sockaddr_un *)addr, addrlen);
        ret = (ret == 0) ? 0 : -EINVAL;
        goto out;
    }

    /* AF_CAN: dispatch to CAN getsockname */
    if (s->domain == AF_CAN) {
        if (*addrlen < sizeof(struct sockaddr_can)) {
            ret = -EINVAL;
            goto out;
        }
        struct sockaddr_can *can_addr = (struct sockaddr_can *)addr;
        ret = can_getsockname(sockfd, can_addr);
        if (ret == 0)
            *addrlen = sizeof(struct sockaddr_can);
        ret = (ret == 0) ? 0 : -EOPNOTSUPP;
        goto out;
    }

    /* AF_PACKET: dispatch to raw packet getsockname */
    if (s->domain == AF_PACKET || (s->domain == 0 && s->type == SOCK_RAW)) {
        if (*addrlen < sizeof(struct sockaddr_ll)) {
            ret = -EINVAL;
            goto out;
        }
        struct sockaddr_ll *sll = (struct sockaddr_ll *)addr;
        ret = packet_getsockname(sockfd, sll);
        if (ret == 0)
            *addrlen = sizeof(struct sockaddr_ll);
        ret = (ret == 0) ? 0 : -EOPNOTSUPP;
        goto out;
    }

    if (*addrlen < sizeof(struct sockaddr_in)) {
        ret = -EINVAL;
        goto out;
    }
    addr->sin_family = AF_INET;
    addr->sin_port = htons(s->local_port);
    addr->sin_addr.s_addr = s->local_ip;
    *addrlen = sizeof(struct sockaddr_in);

out:
    sock_put(s);
    return ret;
}

/**
 * sys_getpeername_impl - Retrieve the remote peer address of a connected socket
 * @sockfd: Socket file descriptor
 * @addr: Buffer to receive the peer sockaddr_in
 * @addrlen: In/out length of @addr
 *
 * Implements the getpeername(2) syscall for AF_INET sockets.
 */
int sys_getpeername_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return -EBADF;
    if (s->state != SOCK_STATE_CONNECTED) {
        sock_put(s);
        return -ENOTCONN;
    }

    int ret = 0;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX && s->unix_ep >= 0) {
        ret = unix_getpeername(s->unix_ep, (struct sockaddr_un *)addr, addrlen);
        ret = (ret == 0) ? 0 : -EINVAL;
        goto out;
    }

    if (*addrlen < sizeof(struct sockaddr_in)) {
        ret = -EINVAL;
        goto out;
    }
    addr->sin_family = AF_INET;
    addr->sin_port = htons(s->remote_port);
    addr->sin_addr.s_addr = s->remote_ip;
    *addrlen = sizeof(struct sockaddr_in);

out:
    sock_put(s);
    return ret;
}

/**
 * sys_socketpair_impl - Create an unnamed connected socket pair
 * @domain: Address family
 * @type: Socket type
 * @protocol: Protocol
 * @sv: int[2] buffer receiving the two new file descriptors
 *
 * Implements the socketpair(2) syscall. Creates two connected AF_UNIX sockets and returns their fds
 * in @sv.
 */
int sys_socketpair_impl(int domain, int type, int protocol, int sv[2]) {
    (void)protocol;

    /* AF_UNIX socket pairs */
    if (domain == AF_UNIX) {
        /* Support SOCK_STREAM and SOCK_DGRAM and SOCK_SEQPACKET */
        if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET)
            return -EOPNOTSUPP;

        /* Allocate two socket slots */
        int slot0 = sock_alloc();
        if (slot0 < 0)
            return -ENOMEM;

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
        s0->domain = AF_UNIX;
        s0->type = type;
        s0->state = SOCK_STATE_CONNECTED;
        s0->unix_ep = ep0;

        /* Set up socket 1 */
        struct socket *s1 = &socket_table[slot1];
        s1->domain = AF_UNIX;
        s1->type = type;
        s1->state = SOCK_STATE_CONNECTED;
        s1->unix_ep = ep1;

        sv[0] = sock_fd_from_slot(slot0);
        sv[1] = sock_fd_from_slot(slot1);
        return 0;
    }

    /* AF_INET socketpair: create a TCP loopback pair */
    if (domain == AF_INET && type == SOCK_STREAM) {
        /* Allocate two socket slots */
        int slot0 = sock_alloc();
        if (slot0 < 0)
            return -ENOMEM;

        struct socket *s0 = &socket_table[slot0];
        s0->domain = AF_INET;
        s0->type = SOCK_STREAM;
        s0->protocol = IPPROTO_TCP;
        s0->state = SOCK_STATE_BOUND;
        s0->local_port = 0; /* ephemeral */

        /* Bind to a random port */
        s0->local_port =
            (uint16_t)(30000 + ((uint32_t)(uintptr_t)s0 ^ (uint32_t)timer_get_ticks()) % 10000);
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
        s1->domain = AF_INET;
        s1->type = SOCK_STREAM;
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

/**
 * sock_poll - Poll a socket for readiness
 * @sockfd: Socket file descriptor
 * @events: Requested events mask (POLLIN | POLLOUT)
 * @pt: Optional poll_table for register/wake integration
 *
 * Implements the poll(2) backend for sockets. Returns a bitmask of
 * POLLIN|POLLOUT|POLLHUP|POLLERR|POLLNVAL reflecting the socket's current readiness.
 */
int sock_poll(int sockfd, int events, struct poll_table *pt) {
    struct socket *s = sock_get(sockfd);
    if (!s)
        return POLLNVAL;

    int revents = 0;

    /* AF_UNIX: dispatch to local socket handler */
    if (s->domain == AF_UNIX && s->unix_ep >= 0) {
        int ret = unix_poll(s->unix_ep, events);
        sock_put(s);
        return ret;
    }

    /* AF_CAN: dispatch to CAN poll handler */
    if (s->domain == AF_CAN) {
        revents = can_poll(sockfd);
        sock_put(s);
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
            if (events & POLLIN)
                revents |= POLLIN;
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

    sock_put(s);
    return result;
}

/* ── Exported symbols for network protocol/driver modules ─────────── */
EXPORT_SYMBOL(socket_init);
EXPORT_SYMBOL(sock_get);
EXPORT_SYMBOL(sock_put);
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
 *
 * Safe to call from softirq context.  Does not acquire socket locks
 * because it only reads in_use/conn_id (aligned ints — atomic on x86)
 * and calls wait_queue_wake_all which has its own spinlock.
 */
/**
 * sock_wake_by_conn_id - Wake socket waitqueues for a TCP connection
 * @conn_id: TCP connection id
 *
 * Called from the TCP stack when data arrives, waking any poll/select/epoll waiters associated with
 * @conn_id.
 */
void sock_wake_by_conn_id(int conn_id) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (socket_table[i].in_use && socket_table[i].conn_id == conn_id) {
            wait_queue_wake_all(&socket_table[i].wq);
        }
    }
}

EXPORT_SYMBOL(sys_setsockopt_impl);
EXPORT_SYMBOL(sys_getsockopt_impl);

/* Export socket poll for protocol modules */
EXPORT_SYMBOL(sock_poll);

/* ── Implement: socket_create ─────────────────────────── */
static int socket_create(int family, int type, int proto) {
    return sys_socket_impl(family, type, proto);
}
/* ── Implement: socket_bind ───────────────────────────── */
static int socket_bind(int sock, const void *addr, int addrlen) {
    return sys_bind_impl(sock, (const struct sockaddr_in *)addr, addrlen);
}
/* ── Implement: socket_listen ─────────────────────────── */
static int socket_listen(int sock, int backlog) {
    return sys_listen_impl(sock, backlog);
}
/* ── Implement: socket_accept ─────────────────────────── */
static int socket_accept(int sock, void *addr, void *addrlen) {
    return sys_accept_impl(sock, (struct sockaddr_in *)addr, (uint32_t *)addrlen);
}
