/*
 * af_unix.c — AF_UNIX (UNIX domain) local sockets
 *
 * Implements SOCK_STREAM communication between processes on the same
 * machine via filesystem path-based addressing.
 *
 * Stream connections use a shared ring buffer for data transfer.
 * Path binding uses a hash table mapping paths to listening endpoints.
 *
 * Reference: Linux AF_UNIX implementation, POSIX.1-2001.
 */
#define KERNEL_INTERNAL
#include "types.h"
#include "socket.h"
#include "process.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "heap.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define UNIX_MAX_PATHS      16      /* maximum number of bound paths */
#define UNIX_MAX_PENDING    8       /* max pending connections per listener */
#define UNIX_BUF_SIZE       4096    /* per-connection data buffer size */
#define UNIX_MAX_ENDPOINTS  16      /* maximum concurrent endpoints */

/* ── Socket states for AF_UNIX ────────────────────────────────────── */

enum unix_state {
    UNIX_FREE      = 0,
    UNIX_BOUND     = 1,     /* bind() called, path registered */
    UNIX_LISTENING = 2,     /* listen() called, accepting connections */
    UNIX_CONNECTED = 3,     /* connected to a peer */
};

/*
 * A connection between two AF_UNIX STREAM endpoints.
 * Uses a single shared ring buffer split logically into two halves:
 *   - Server writes to the A-region, Client reads from the A-region
 *   - Client writes to the B-region, Server reads from the B-region
 * A and B share the same physical buffer for simplicity.
 */
struct unix_conn {
    spinlock_t  lock;
    uint8_t    *buf;           /* shared buffer */
    uint32_t    size;          /* total buffer capacity */
    /* Server→Client direction */
    uint32_t    wpos_a;        /* server write position */
    uint32_t    rpos_a;        /* client read position */
    int         closed_a;      /* server closed its write side */
    /* Client→Server direction */
    uint32_t    wpos_b;        /* client write position */
    uint32_t    rpos_b;        /* server read position */
    int         closed_b;      /* client closed its write side */
    int         refs;          /* number of endpoints referencing this */
};

/* ── UNIX endpoint (one side of a connection) ──────────────────────── */

struct unix_ep {
    int                    in_use;
    int                    type;        /* SOCK_STREAM */
    enum unix_state        state;
    char                   bound_path[UNIX_PATH_MAX];
    struct unix_conn      *conn;        /* non-NULL when connected/listening */
    int                    is_server;   /* 1 = server (accepting) side */
    /* Listener data */
    int                    backlog;
    int                    pending[UNIX_MAX_PENDING]; /* client endpoint ids */
    int                    npending;
};
/* Global state */

#define UNIX_MAX_ENDPOINTS 16
static struct unix_ep    eps[UNIX_MAX_ENDPOINTS];
struct path_entry {
    char  path[UNIX_PATH_MAX];
    int   ep_idx;
    int   in_use;
};
static struct path_entry path_tab[UNIX_MAX_PATHS];
static spinlock_t unix_lock;

/* ── Path table operations ────────────────────────────────────────── */

static uint32_t path_hash(const char *s)
{
    uint32_t h = 0;
    while (*s) h = (h << 5) - h + (uint8_t)*s++;
    return h % UNIX_MAX_PATHS;
}

static struct path_entry *
path_lookup(const char *path)
{
    if (!path || !path[0]) return NULL;
    uint32_t i = path_hash(path), start = i;
    do {
        if (path_tab[i].in_use && strcmp(path_tab[i].path, path) == 0)
            return &path_tab[i];
        i = (i + 1) % UNIX_MAX_PATHS;
    } while (i != start);
    return NULL;
}

static int path_register(const char *path, int ep_idx)
{
    if (!path || !path[0]) return -EINVAL;
    if (path_lookup(path)) return -EADDRINUSE;
    uint32_t i = path_hash(path), start = i;
    do {
        if (!path_tab[i].in_use) {
            uint32_t n = (uint32_t)strlen(path);
            if (n >= UNIX_PATH_MAX) n = UNIX_PATH_MAX - 1;
            memcpy(path_tab[i].path, path, n);
            path_tab[i].path[n] = '\0';
            path_tab[i].ep_idx = ep_idx;
            path_tab[i].in_use = 1;
            return 0;
        }
        i = (i + 1) % UNIX_MAX_PATHS;
    } while (i != start);
    return -ENOMEM;
}

static void path_unregister(const char *path)
{
    struct path_entry *e;
    if ((e = path_lookup(path))) { e->in_use = 0; memset(e->path, 0, sizeof(e->path)); }
}

/* ── Endpoint management ──────────────────────────────────────────── */

static int ep_alloc(void)
{
    for (int i = 0; i < UNIX_MAX_ENDPOINTS; i++)
        if (!eps[i].in_use) { memset(&eps[i], 0, sizeof(eps[i])); eps[i].in_use = 1; return i; }
    return -1;
}

static struct unix_ep *ep_get(int idx)
{
    return (idx >= 0 && idx < UNIX_MAX_ENDPOINTS && eps[idx].in_use) ? &eps[idx] : NULL;
}

static void ep_free(int idx)
{
    struct unix_ep *ep = ep_get(idx);
    if (!ep) return;
    if (ep->bound_path[0]) { path_unregister(ep->bound_path); vfs_unlink(ep->bound_path); }
    if (ep->conn) {
        struct unix_conn *c = ep->conn;
        spinlock_acquire(&c->lock);
        c->refs--;
        int gone = (c->refs <= 0);
        spinlock_release(&c->lock);
        if (gone) { kfree(c->buf); kfree(c); }
        ep->conn = NULL;
    }
    memset(ep, 0, sizeof(*ep));
}

/* ── Connection allocation ────────────────────────────────────────── */

static struct unix_conn *conn_alloc(void)
{
    struct unix_conn *c = (struct unix_conn *)kmalloc(sizeof(*c));
    if (!c) return NULL;
    c->buf = (uint8_t *)kmalloc(UNIX_BUF_SIZE);
    if (!c->buf) { kfree(c); return NULL; }
    memset(c->buf, 0, UNIX_BUF_SIZE);
    c->size = UNIX_BUF_SIZE;
    c->wpos_a = c->rpos_a = c->wpos_b = c->rpos_b = 0;
    c->closed_a = c->closed_b = 0;
    c->refs = 2; /* server + client */
    spinlock_init(&c->lock);
    return c;
}

/* ── Circular buffer helpers ──────────────────────────────────────── */

static uint32_t buf_used(uint32_t wp, uint32_t rp, uint32_t sz)
{ return (wp >= rp) ? (wp - rp) : (sz - rp + wp); }

static uint32_t buf_free(uint32_t wp, uint32_t rp, uint32_t sz)
{ return sz - buf_used(wp, rp, sz) - 1; }

static uint32_t buf_write(uint8_t *b, uint32_t *wp, uint32_t rp, uint32_t sz,
                          const uint8_t *d, uint32_t len)
{
    uint32_t n = 0;
    while (len) {
        uint32_t f = buf_free(*wp, rp, sz);
        if (!f) break;
        uint32_t c = (len < f) ? len : f;
        uint32_t e = sz - *wp;
        if (c > e) c = e;
        memcpy(b + *wp, d + n, c);
        *wp = (*wp + c) % sz;
        n += c; len -= c;
    }
    return n;
}

static uint32_t buf_read(const uint8_t *b, uint32_t wp, uint32_t *rp, uint32_t sz,
                         uint8_t *d, uint32_t len)
{
    uint32_t n = 0;
    while (len) {
        uint32_t u = buf_used(wp, *rp, sz);
        if (!u) break;
        uint32_t c = (len < u) ? len : u;
        uint32_t e = sz - *rp;
        if (c > e) c = e;
        memcpy(d + n, b + *rp, c);
        *rp = (*rp + c) % sz;
        n += c; len -= c;
    }
    return n;
}

/* ════════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════════ */

int unix_create(int type)
{
    if (type != SOCK_STREAM) return -EINVAL;
    spinlock_acquire(&unix_lock);
    int idx = ep_alloc();
    spinlock_release(&unix_lock);
    if (idx >= 0) { eps[idx].type = SOCK_STREAM; eps[idx].state = UNIX_FREE; }
    return idx;
}

void unix_destroy(int idx)
{
    spinlock_acquire(&unix_lock);
    ep_free(idx);
    spinlock_release(&unix_lock);
}

int unix_bind(int idx, const struct sockaddr_un *addr, uint32_t addrlen)
{
    if (!addr) return -EFAULT;
    if (addr->sun_family != AF_UNIX) return -EINVAL;
    char path[UNIX_PATH_MAX];
    int plen = (int)addrlen - (int)sizeof(uint16_t);
    if (plen <= 0) return -EINVAL;
    if (plen >= UNIX_PATH_MAX) plen = UNIX_PATH_MAX - 1;
    memcpy(path, addr->sun_path, (size_t)plen);
    path[plen] = '\0';
    if (!path[0]) return -ENOENT;

    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return -EBADF; }
    if (ep->state != UNIX_FREE) { spinlock_release(&unix_lock); return -EINVAL; }

    /* Check for stale socket file */
    struct vfs_stat st;
    if (vfs_stat(path, &st) == 0) {
        if (path_lookup(path)) { spinlock_release(&unix_lock); return -EADDRINUSE; }
        vfs_unlink(path);
    }

    /* Create the socket file as a marker.
     * vfs_create() returns an fd; we close it via sys_close internals.
     * If that fails, we still register the path (the path table entry
     * is what matters for bind/connect). */
    vfs_create(path, 0);
    /* Ignore errors — the socket file existence is advisory */

    int ret = path_register(path, idx);
    if (ret < 0) { vfs_unlink(path); spinlock_release(&unix_lock); return ret; }
    memcpy(ep->bound_path, path, (size_t)plen + 1);
    ep->state = UNIX_BOUND;
    spinlock_release(&unix_lock);
    return 0;
}

int unix_listen(int idx, int backlog)
{
    if (backlog <= 0) backlog = 1;
    if (backlog > UNIX_MAX_PENDING) backlog = UNIX_MAX_PENDING;
    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return -EBADF; }
    if (ep->state != UNIX_BOUND) { spinlock_release(&unix_lock); return -EINVAL; }
    if (ep->type != SOCK_STREAM) { spinlock_release(&unix_lock); return -EOPNOTSUPP; }
    ep->backlog = backlog;
    ep->state = UNIX_LISTENING;
    spinlock_release(&unix_lock);
    return 0;
}

int unix_accept(int idx, int timeout_ms)
{
    (void)timeout_ms;
    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return -EBADF; }
    if (ep->state != UNIX_LISTENING) { spinlock_release(&unix_lock); return -EINVAL; }

    /* Non-blocking: return immediately if nothing is pending */
    if (ep->npending == 0) { spinlock_release(&unix_lock); return -EAGAIN; }

    /* Pop first pending client */
    int cidx = ep->pending[0];
    for (int i = 0; i < ep->npending - 1; i++) ep->pending[i] = ep->pending[i + 1];
    ep->npending--;
    struct unix_ep *cep = ep_get(cidx);
    if (cep) cep->state = UNIX_CONNECTED;
    spinlock_release(&unix_lock);
    return cidx;
}

int unix_connect(int idx, const struct sockaddr_un *addr, uint32_t addrlen)
{
    if (!addr) return -EFAULT;
    char path[UNIX_PATH_MAX];
    int plen = (int)addrlen - (int)sizeof(uint16_t);
    if (plen <= 0) return -EINVAL;
    if (plen >= UNIX_PATH_MAX) plen = UNIX_PATH_MAX - 1;
    memcpy(path, addr->sun_path, (size_t)plen);
    path[plen] = '\0';

    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return -EBADF; }

    /* Look up the listener */
    struct path_entry *pe = path_lookup(path);
    if (!pe) { spinlock_release(&unix_lock); return -ECONNREFUSED; }
    struct unix_ep *sep = ep_get(pe->ep_idx);
    if (!sep || sep->state != UNIX_LISTENING) { spinlock_release(&unix_lock); return -ECONNREFUSED; }
    if (sep->npending >= sep->backlog) { spinlock_release(&unix_lock); return -ECONNREFUSED; }

    /* Allocate connection object */
    struct unix_conn *c = conn_alloc();
    if (!c) { spinlock_release(&unix_lock); return -ENOMEM; }

    ep->conn = c;
    ep->is_server = 0;
    ep->state = UNIX_CONNECTED;

    sep->conn = c;
    sep->is_server = 1;
    sep->pending[sep->npending++] = idx;

    spinlock_release(&unix_lock);
    return 0;
}

int unix_send(int idx, const void *data, uint32_t len, int nonblock)
{
    if (!data || !len) return 0;
    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return -EBADF; }
    struct unix_conn *c = ep->conn;
    if (!c) { spinlock_release(&unix_lock); return -ENOTCONN; }

    spinlock_acquire(&c->lock);
    spinlock_release(&unix_lock); /* drop global, hold conn lock */

    uint32_t *wp, *rp;
    int *eof_peer;
    if (ep->is_server) { wp = &c->wpos_a; rp = &c->rpos_a; eof_peer = &c->closed_b; }
    else               { wp = &c->wpos_b; rp = &c->rpos_b; eof_peer = &c->closed_a; }

    if (*eof_peer) { spinlock_release(&c->lock); return -EPIPE; }

    uint32_t written = buf_write(c->buf, wp, *rp, c->size, (const uint8_t *)data, len);
    spinlock_release(&c->lock);
    if (written == 0 && len > 0 && nonblock) return -EAGAIN;
    return (int)written;
}

int unix_recv(int idx, void *data, uint32_t len, int nonblock)
{
    if (!data || !len) return 0;
    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return -EBADF; }
    struct unix_conn *c = ep->conn;
    if (!c) { spinlock_release(&unix_lock); return -ENOTCONN; }

    spinlock_acquire(&c->lock);
    spinlock_release(&unix_lock);

    uint32_t *wp, *rp;
    int *eof_peer;
    if (ep->is_server) { wp = &c->wpos_b; rp = &c->rpos_b; eof_peer = &c->closed_b; }
    else               { wp = &c->wpos_a; rp = &c->rpos_a; eof_peer = &c->closed_a; }

    /* Check if data available */
    uint32_t used = buf_used(*wp, *rp, c->size);
    if (used == 0) {
        if (*eof_peer) { spinlock_release(&c->lock); return 0; } /* EOF */
        if (nonblock) { spinlock_release(&c->lock); return -EAGAIN; }
        spinlock_release(&c->lock);
        return -EAGAIN; /* non-blocking only for now */
    }

    uint32_t n = buf_read(c->buf, *wp, rp, c->size, (uint8_t *)data, len);
    spinlock_release(&c->lock);
    return (int)n;
}

int unix_shutdown(int idx, int how)
{
    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return -EBADF; }
    if (ep->conn) {
        struct unix_conn *c = ep->conn;
        spinlock_acquire(&c->lock);
        if (how == SHUT_WR || how == SHUT_RDWR) {
            if (ep->is_server) c->closed_a = 1;
            else               c->closed_b = 1;
        }
        spinlock_release(&c->lock);
    }
    spinlock_release(&unix_lock);
    return 0;
}

int unix_poll(int idx, int events)
{
    int rev = 0;
    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return POLLNVAL; }

    if (ep->state == UNIX_LISTENING) {
        rev |= POLLOUT;
        if (ep->npending > 0) rev |= POLLIN;
        spinlock_release(&unix_lock);
        return rev & events;
    }

    struct unix_conn *c = ep->conn;
    if (!c) { spinlock_release(&unix_lock); return POLLHUP; }

    spinlock_acquire(&c->lock);
    uint32_t *wp_r, *rp_r, *wp_w, *rp_w;
    int *eof_peer;
    if (ep->is_server) {
        wp_r = &c->wpos_b; rp_r = &c->rpos_b; wp_w = &c->wpos_a; rp_w = &c->rpos_a;
        eof_peer = &c->closed_b;
    } else {
        wp_r = &c->wpos_a; rp_r = &c->rpos_a; wp_w = &c->wpos_b; rp_w = &c->rpos_b;
        eof_peer = &c->closed_a;
    }

    if (buf_used(*wp_r, *rp_r, c->size) > 0) rev |= POLLIN;
    if (*eof_peer) rev |= POLLIN | POLLHUP;
    if (buf_free(*wp_w, *rp_w, c->size) > 0) rev |= POLLOUT;
    if (*eof_peer) rev |= POLLERR;

    spinlock_release(&c->lock);
    spinlock_release(&unix_lock);
    return rev & events;
}

int unix_getsockname(int idx, struct sockaddr_un *addr, uint32_t *addrlen)
{
    if (!addr || !addrlen) return -EFAULT;
    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return -EBADF; }
    if (!ep->bound_path[0]) { spinlock_release(&unix_lock); return -ENOTCONN; }
    uint32_t plen = (uint32_t)strlen(ep->bound_path);
    uint32_t need = (uint32_t)sizeof(uint16_t) + plen;
    if (*addrlen < need) { *addrlen = need; spinlock_release(&unix_lock); return -1; }
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, ep->bound_path, plen);
    if (plen < UNIX_PATH_MAX) addr->sun_path[plen] = '\0';
    *addrlen = need;
    spinlock_release(&unix_lock);
    return 0;
}

int unix_getpeername(int idx, struct sockaddr_un *addr, uint32_t *addrlen)
{
    (void)idx;
    /* Minimal: return empty AF_UNIX address */
    if (addr && *addrlen >= (uint32_t)sizeof(uint16_t)) {
        addr->sun_family = AF_UNIX;
        addr->sun_path[0] = '\0';
        *addrlen = (uint32_t)sizeof(uint16_t);
    }
    return 0;
}

/* ── Initialisation ───────────────────────────────────────────────── */

void af_unix_init(void)
{
    memset(eps, 0, sizeof(eps));
    memset(path_tab, 0, sizeof(path_tab));
    spinlock_init(&unix_lock);
    kprintf("[OK] AF_UNIX local domain sockets initialized\n");
}
