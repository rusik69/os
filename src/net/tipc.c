/*
 * tipc.c — TIPC (Transparent Inter Process Communication) protocol
 *
 * Implements a minimal TIPC protocol stack supporting:
 *   - Native TIPC sockets (AF_TIPC)
 *   - Port identification and addressing
 *   - Connectionless (datagram) and connection-oriented (stream) messaging
 *   - Name table for service resolution
 *
 * TIPC addressing: <zone.node.port> or <service.instance>
 */

#define KERNEL_INTERNAL
#include "socket.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"

#define AF_TIPC          30
#define TIPC_PORT_MAX    1024
#define TIPC_NAME_MAX    64
#define TIPC_NODE_MAX    16

/* TIPC address structure */
struct tipc_addr {
    uint32_t zone;
    uint32_t node;
    uint32_t port;
};

/* TIPC socket */
struct tipc_sock {
    int           in_use;
    struct tipc_addr local;
    struct tipc_addr remote;
    int           connected;
    int           type;   /* SOCK_DGRAM or SOCK_STREAM */
};

/* Name table entry */
struct tipc_name_entry {
    int      in_use;
    char     name[TIPC_NAME_MAX];
    uint32_t port;
    uint32_t instance;
};

static struct tipc_sock g_tipc_socks[TIPC_PORT_MAX];
static spinlock_t g_tipc_lock;
static int g_tipc_initialized;

/* ── Socket operations ─────────────────────────────────────────────── */

int tipc_socket(int type)
{
    if (type != SOCK_DGRAM && type != SOCK_STREAM)
        return -EINVAL;

    spinlock_acquire(&g_tipc_lock);
    for (uint32_t port = 1; port < TIPC_PORT_MAX; port++) {
        if (!g_tipc_socks[port].in_use) {
            struct tipc_sock *s = &g_tipc_socks[port];
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            s->type = type;
            s->local.zone = 1;
            s->local.node = 1;
            s->local.port = port;
            spinlock_release(&g_tipc_lock);
            return (int)port;
        }
    }
    spinlock_release(&g_tipc_lock);
    return -ENOSPC;
}

int tipc_bind(int sock, struct tipc_addr *addr)
{
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use)
        return -EBADF;
    struct tipc_sock *s = &g_tipc_socks[sock];
    if (addr) {
        s->local = *addr;
    }
    return 0;
}

int tipc_sendmsg(int sock, const void *buf, uint32_t len, struct tipc_addr *dest)
{
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use)
        return -EBADF;
    if (!buf || len == 0) return -EINVAL;
    /* Stub: would route message across cluster */
    (void)dest;
    return (int)len;
}

int tipc_recvmsg(int sock, void *buf, uint32_t len, struct tipc_addr *src)
{
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use)
        return -EBADF;
    if (!buf || len == 0) return -EINVAL;
    /* Stub: would receive message from cluster */
    (void)src;
    return -EAGAIN;
}

int tipc_close(int sock)
{
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use)
        return -EBADF;
    spinlock_acquire(&g_tipc_lock);
    memset(&g_tipc_socks[sock], 0, sizeof(struct tipc_sock));
    spinlock_release(&g_tipc_lock);
    return 0;
}

/* ── Name table ──────────────────────────────────────────────────────── */

static struct tipc_name_entry g_tipc_names[TIPC_NAME_MAX];

int tipc_publish(const char *name, uint32_t instance, uint32_t port)
{
    if (!name) return -EINVAL;
    spinlock_acquire(&g_tipc_lock);
    for (int i = 0; i < TIPC_NAME_MAX; i++) {
        if (!g_tipc_names[i].in_use) {
            strncpy(g_tipc_names[i].name, name, TIPC_NAME_MAX - 1);
            g_tipc_names[i].instance = instance;
            g_tipc_names[i].port = port;
            g_tipc_names[i].in_use = 1;
            spinlock_release(&g_tipc_lock);
            return 0;
        }
    }
    spinlock_release(&g_tipc_lock);
    return -ENOSPC;
}

int tipc_lookup(const char *name, uint32_t instance, uint32_t *port)
{
    if (!name || !port) return -EINVAL;
    spinlock_acquire(&g_tipc_lock);
    for (int i = 0; i < TIPC_NAME_MAX; i++) {
        if (g_tipc_names[i].in_use &&
            strcmp(g_tipc_names[i].name, name) == 0 &&
            g_tipc_names[i].instance == instance) {
            *port = g_tipc_names[i].port;
            spinlock_release(&g_tipc_lock);
            return 0;
        }
    }
    spinlock_release(&g_tipc_lock);
    return -ENOENT;
}

/* ── Initialization ─────────────────────────────────────────────────── */

void tipc_init(void)
{
    if (g_tipc_initialized) return;
    memset(g_tipc_socks, 0, sizeof(g_tipc_socks));
    memset(g_tipc_names, 0, sizeof(g_tipc_names));
    spinlock_init(&g_tipc_lock);
    g_tipc_initialized = 1;
    kprintf("[OK] TIPC protocol initialized\n");
}

/* ── Implement: tipc_send_msg ────────────────── */
int tipc_send_msg(int sock, const void *buf, uint32_t len, struct tipc_addr *dest)
{
    if (!g_tipc_initialized) {
        kprintf("[tipc] tipc_send_msg: not initialized\n");
        return -ENOSYS;
    }
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use) {
        kprintf("[tipc] tipc_send_msg: invalid socket %d\n", sock);
        return -EBADF;
    }
    if (!buf || len == 0) {
        kprintf("[tipc] tipc_send_msg: invalid buffer (ptr=%p len=%u)\n",
                (const void *)buf, (unsigned)len);
        return -EINVAL;
    }
    if (!dest) {
        kprintf("[tipc] tipc_send_msg: NULL destination\n");
        return -EINVAL;
    }
    if (!g_tipc_socks[sock].connected && g_tipc_socks[sock].type == SOCK_STREAM) {
        kprintf("[tipc] tipc_send_msg: socket %d not connected\n", sock);
        return -ENOTCONN;
    }
    kprintf("[tipc] tipc_send_msg: sock=%d len=%u to zone=%u node=%u port=%u (stub)\n",
            sock, (unsigned)len, dest->zone, dest->node, dest->port);
    return -EOPNOTSUPP;
}

/* ── Implement: tipc_recv_msg ────────────────── */
int tipc_recv_msg(int sock, void *buf, uint32_t len, struct tipc_addr *src)
{
    if (!g_tipc_initialized) {
        kprintf("[tipc] tipc_recv_msg: not initialized\n");
        return -ENOSYS;
    }
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use) {
        kprintf("[tipc] tipc_recv_msg: invalid socket %d\n", sock);
        return -EBADF;
    }
    if (!buf || len == 0) {
        kprintf("[tipc] tipc_recv_msg: invalid buffer\n");
        return -EINVAL;
    }
    kprintf("[tipc] tipc_recv_msg: sock=%d len=%u (stub)\n", sock, (unsigned)len);
    return -EOPNOTSUPP;
}

/* ── Implement: tipc_connect ────────────────── */
int tipc_connect(int sock, struct tipc_addr *addr)
{
    if (!g_tipc_initialized) {
        kprintf("[tipc] tipc_connect: not initialized\n");
        return -ENOSYS;
    }
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use) {
        kprintf("[tipc] tipc_connect: invalid socket %d\n", sock);
        return -EBADF;
    }
    if (!addr) {
        kprintf("[tipc] tipc_connect: NULL address\n");
        return -EINVAL;
    }
    if (g_tipc_socks[sock].connected) {
        kprintf("[tipc] tipc_connect: socket %d already connected\n", sock);
        return -EISCONN;
    }
    if (g_tipc_socks[sock].type == SOCK_DGRAM) {
        /* For SOCK_DGRAM, connect just sets the default destination */
        g_tipc_socks[sock].remote = *addr;
        g_tipc_socks[sock].connected = 1;
        kprintf("[tipc] tipc_connect: sock=%d connected to zone=%u node=%u port=%u\n",
                sock, addr->zone, addr->node, addr->port);
        return 0;
    }
    kprintf("[tipc] tipc_connect: sock=%d to zone=%u node=%u port=%u (stub)\n",
            sock, addr->zone, addr->node, addr->port);
    g_tipc_socks[sock].remote = *addr;
    g_tipc_socks[sock].connected = 1;
    return 0;
}

/* ── Implement: tipc_disconnect ────────────────── */
int tipc_disconnect(int sock)
{
    if (!g_tipc_initialized) {
        kprintf("[tipc] tipc_disconnect: not initialized\n");
        return -ENOSYS;
    }
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use) {
        kprintf("[tipc] tipc_disconnect: invalid socket %d\n", sock);
        return -EBADF;
    }
    if (!g_tipc_socks[sock].connected) {
        kprintf("[tipc] tipc_disconnect: socket %d not connected\n", sock);
        return -ENOTCONN;
    }
    spinlock_acquire(&g_tipc_lock);
    g_tipc_socks[sock].connected = 0;
    memset(&g_tipc_socks[sock].remote, 0, sizeof(struct tipc_addr));
    spinlock_release(&g_tipc_lock);
    kprintf("[tipc] tipc_disconnect: sock=%d disconnected\n", sock);
    return 0;
}

/* ── Implement: tipc_shutdown ────────────────── */
int tipc_shutdown(int sock, int how)
{
    if (!g_tipc_initialized) {
        kprintf("[tipc] tipc_shutdown: not initialized\n");
        return -ENOSYS;
    }
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use) {
        kprintf("[tipc] tipc_shutdown: invalid socket %d\n", sock);
        return -EBADF;
    }
    if (how < 0 || how > 2) {
        kprintf("[tipc] tipc_shutdown: invalid 'how' %d\n", how);
        return -EINVAL;
    }
    spinlock_acquire(&g_tipc_lock);
    /* how=0: no more reads, 1: no more writes, 2: no more reads/writes */
    if (how == 0 || how == 2) {
        /* Shutdown reads — nothing to do for now */
    }
    if (how == 1 || how == 2) {
        /* Shutdown writes — flush pending data */
    }
    spinlock_release(&g_tipc_lock);
    kprintf("[tipc] tipc_shutdown: sock=%d how=%d (stub)\n", sock, how);
    return 0;
}

/* ── Implement: tipc_listen ────────────────── */
int tipc_listen(int sock, int backlog)
{
    if (!g_tipc_initialized) {
        kprintf("[tipc] tipc_listen: not initialized\n");
        return -ENOSYS;
    }
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use) {
        kprintf("[tipc] tipc_listen: invalid socket %d\n", sock);
        return -EBADF;
    }
    if (backlog < 0) {
        kprintf("[tipc] tipc_listen: invalid backlog %d\n", backlog);
        return -EINVAL;
    }
    if (g_tipc_socks[sock].type != SOCK_STREAM) {
        kprintf("[tipc] tipc_listen: socket %d not SOCK_STREAM\n", sock);
        return -EOPNOTSUPP;
    }
    kprintf("[tipc] tipc_listen: sock=%d backlog=%d (stub)\n", sock, backlog);
    return -EOPNOTSUPP;
}

/* ── Implement: tipc_accept ────────────────── */
int tipc_accept(int sock, struct tipc_addr *addr)
{
    if (!g_tipc_initialized) {
        kprintf("[tipc] tipc_accept: not initialized\n");
        return -ENOSYS;
    }
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use) {
        kprintf("[tipc] tipc_accept: invalid socket %d\n", sock);
        return -EBADF;
    }
    if (!addr) {
        kprintf("[tipc] tipc_accept: NULL address\n");
        return -EINVAL;
    }
    kprintf("[tipc] tipc_accept: sock=%d (stub)\n", sock);
    return -EOPNOTSUPP;
}

/* ── Implement: tipc_setsockopt ────────────────── */
int tipc_setsockopt(int sock, int level, int optname, const void *optval, uint32_t optlen)
{
    if (!g_tipc_initialized) {
        kprintf("[tipc] tipc_setsockopt: not initialized\n");
        return -ENOSYS;
    }
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use) {
        kprintf("[tipc] tipc_setsockopt: invalid socket %d\n", sock);
        return -EBADF;
    }
    if (!optval || optlen == 0) {
        kprintf("[tipc] tipc_setsockopt: invalid optval\n");
        return -EINVAL;
    }
    kprintf("[tipc] tipc_setsockopt: sock=%d level=%d optname=%d (stub)\n",
            sock, level, optname);
    return -EOPNOTSUPP;
}

/* ── Implement: tipc_getsockopt ────────────────── */
int tipc_getsockopt(int sock, int level, int optname, void *optval, uint32_t *optlen)
{
    if (!g_tipc_initialized) {
        kprintf("[tipc] tipc_getsockopt: not initialized\n");
        return -ENOSYS;
    }
    if (sock < 0 || sock >= TIPC_PORT_MAX || !g_tipc_socks[sock].in_use) {
        kprintf("[tipc] tipc_getsockopt: invalid socket %d\n", sock);
        return -EBADF;
    }
    if (!optval || !optlen || *optlen == 0) {
        kprintf("[tipc] tipc_getsockopt: invalid output buffer\n");
        return -EINVAL;
    }
    kprintf("[tipc] tipc_getsockopt: sock=%d level=%d optname=%d (stub)\n",
            sock, level, optname);
    return -EOPNOTSUPP;
}
