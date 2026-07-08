/*
 * af_unix.c — AF_UNIX (UNIX domain) local sockets
 *
 * Implements SOCK_STREAM and SOCK_DGRAM communication between processes
 * on the same machine via filesystem path-based addressing (and the
 * abstract namespace).
 *
 * SOCK_STREAM: connection-oriented, sequenced, reliable byte stream.
 *   Uses a shared ring buffer for data transfer.
 *
 * SOCK_DGRAM: connectionless, message-boundary-preserving datagrams.
 *   Each message is queued as a discrete datagram with sender address.
 *
 * Ancillary data: SCM_RIGHTS (file descriptor passing) and
 *   SCM_CREDENTIALS (process credentials) are supported on both
 *   SOCK_STREAM and SOCK_DGRAM via sendmsg/recvmsg.
 *
 * Path binding uses a hash table mapping paths to listening endpoints.
 * Abstract namespace (sun_path[0] == '\0') is also supported.
 *
 * Reference: Linux AF_UNIX implementation, POSIX.1-2001.
 */
#define KERNEL_INTERNAL
#include "types.h"
#include "socket.h"
#include "af_unix.h"
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
#define UNIX_DGRAM_QUEUE    64      /* max queued datagrams per endpoint */
#define UNIX_DGRAM_MAX      8192    /* max datagram payload size */
#define UNIX_CRED_SIZE      (sizeof(struct ucred) + sizeof(struct cmsghdr))

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

/* Datagram message — stores one discrete message for SOCK_DGRAM */
struct unix_dgram {
    uint32_t    len;                /* payload length */
    char        peer_path[UNIX_PATH_MAX]; /* sender's bound path (if any) */
    uint8_t    *data;               /* kmalloc'd payload */
    /* Ancillary data carried with this datagram */
    int         has_creds;
    struct ucred creds;
    int         nfds;               /* number of passed fds */
    int         fds[8];             /* passed file descriptors (max 8) */
};

/* ── UNIX endpoint (one side of a connection) ──────────────────────── */

struct unix_ep {
    int                    in_use;
    int                    type;        /* SOCK_STREAM or SOCK_DGRAM */
    enum unix_state        state;
    char                   bound_path[UNIX_PATH_MAX];
    struct unix_conn      *conn;        /* non-NULL when connected/listening */
    int                    is_server;   /* 1 = server (accepting) side */
    /* Listener data */
    int                    backlog;
    int                    pending[UNIX_MAX_PENDING]; /* client endpoint ids */
    int                    npending;
    /* DGRAM queue (for SOCK_DGRAM endpoints) */
    struct unix_dgram      dgram_queue[UNIX_DGRAM_QUEUE];
    volatile int           dgram_head;  /* producer index */
    volatile int           dgram_tail;  /* consumer index */
    /* Default destination for connected DGRAM sockets */
    char                   default_dst[UNIX_PATH_MAX];
    int                    default_dst_len;
    /* Stream ancillary data (pending creds for next recvmsg on SOCK_STREAM) */
    int                    has_pending_creds;
    struct ucred           pending_creds;
    int                    pending_nfds;
    int                    pending_fds[8];
};
/* Global state */

#define UNIX_MAX_ENDPOINTS 16
static struct unix_ep    eps[UNIX_MAX_ENDPOINTS];
struct path_entry {
    char  path[UNIX_PATH_MAX];
    int   path_len;          /* number of bytes in path (can include embedded nul) */
    int   ep_idx;
    int   in_use;
};
static struct path_entry path_tab[UNIX_MAX_PATHS];
static spinlock_t unix_lock;

/* ── Path table operations ────────────────────────────────────────── */

/* Hash a byte array of given length */
static uint32_t path_hash_len(const char *s, int len)
{
    uint32_t h = 0;
    for (int i = 0; i < len; i++) h = (h << 5) - h + (uint8_t)s[i];
    return h % UNIX_MAX_PATHS;
}

static uint32_t path_hash(const char *s)
{
    return path_hash_len(s, (int)strlen(s));
}

/* Compare two path entries up to their stored lengths */
static int path_eq(const struct path_entry *e, const char *path, int len)
{
    if (e->path_len != len) return 0;
    return memcmp(e->path, path, (size_t)len) == 0;
}

static struct path_entry *path_lookup_len(const char *path, int len)
{
    if (!path || len <= 0) return NULL;
    uint32_t i = path_hash_len(path, len), start = i;
    do {
        if (path_tab[i].in_use && path_eq(&path_tab[i], path, len))
            return &path_tab[i];
        i = (i + 1) % UNIX_MAX_PATHS;
    } while (i != start);
    return NULL;
}

static struct path_entry *path_lookup(const char *path)
{
    return path_lookup_len(path, (int)strlen(path));
}

static int path_register(const char *path, int path_len, int ep_idx)
{
    if (!path || path_len <= 0) return -EINVAL;
    if (path_lookup_len(path, path_len)) return -EADDRINUSE;
    uint32_t i = path_hash_len(path, path_len), start = i;
    do {
        if (!path_tab[i].in_use) {
            uint32_t n = (uint32_t)path_len;
            if (n >= UNIX_PATH_MAX) n = UNIX_PATH_MAX - 1;
            memcpy(path_tab[i].path, path, n);
            path_tab[i].path_len = (int)n;
            path_tab[i].path[n] = '\0'; /* nul-terminate for safety */
            path_tab[i].ep_idx = ep_idx;
            path_tab[i].in_use = 1;
            return 0;
        }
        i = (i + 1) % UNIX_MAX_PATHS;
    } while (i != start);
    return -ENOMEM;
}

static void path_unregister(const char *path, int path_len)
{
    struct path_entry *e;
    if ((e = path_lookup_len(path, path_len))) {
        e->in_use = 0;
        memset(e->path, 0, sizeof(e->path));
        e->path_len = 0;
    }
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
    if (ep->bound_path[0] != '\0' || ep->bound_path[1] != '\0') {
        /* Determine path length.  For abstract sockets (starts with \0),
         * calculate length from the whole buffer.  For regular paths,
         * use strlen. */
        int pl;
        if (ep->bound_path[0] == '\0') {
            /* Abstract socket: path is \0 + N bytes.  Find the end. */
            for (pl = 1; pl < UNIX_PATH_MAX && ep->bound_path[pl] != '\0'; pl++)
                ;
        } else {
            pl = (int)strlen(ep->bound_path);
        }
        if (pl > 0) {
            path_unregister(ep->bound_path, pl);
            if (ep->bound_path[0] != '\0')
                vfs_unlink(ep->bound_path);
        }
    }
    if (ep->conn) {
        struct unix_conn *c = ep->conn;
        spinlock_acquire(&c->lock);
        c->refs--;
        int gone = (c->refs <= 0);
        spinlock_release(&c->lock);
        if (gone) { kfree(c->buf); kfree(c); }
        ep->conn = NULL;
    }
    /* Free any pending DGRAM payloads */
    while (ep->dgram_tail != ep->dgram_head) {
        struct unix_dgram *dg = &ep->dgram_queue[ep->dgram_tail];
        if (dg->data) kfree(dg->data);
        dg->data = NULL;
        dg->len = 0;
        ep->dgram_tail = (ep->dgram_tail + 1) % UNIX_DGRAM_QUEUE;
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
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET) return -EINVAL;
    spinlock_acquire(&unix_lock);
    int idx = ep_alloc();
    spinlock_release(&unix_lock);
    if (idx >= 0) {
        eps[idx].type = type;
        eps[idx].state = UNIX_FREE;
        eps[idx].dgram_head = 0;
        eps[idx].dgram_tail = 0;
    }
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
    /* Don't nul-terminate if abstract (starts with \0) — the entire
     * byte sequence is the name.  For regular paths, ensure termination. */
    int path_len = plen;
    if (path[0] != '\0') {
        path[plen] = '\0';
        path_len = (int)strlen(path);
    } else if (plen == 0) {
        return -ENOENT; /* empty abstract name not allowed */
    }

    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return -EBADF; }
    if (ep->state != UNIX_FREE) { spinlock_release(&unix_lock); return -EINVAL; }

    /* For regular (non-abstract) paths, check for stale socket file */
    if (path[0] != '\0') {
        struct vfs_stat st;
        if (vfs_stat(path, &st) == 0) {
            if (path_lookup_len(path, path_len)) { spinlock_release(&unix_lock); return -EADDRINUSE; }
            vfs_unlink(path);
        }
        /* Create the socket file as a marker */
        vfs_create(path, 0);
    } else {
        /* Abstract path: just check it's not already registered */
        if (path_lookup_len(path, path_len)) { spinlock_release(&unix_lock); return -EADDRINUSE; }
    }

    int ret = path_register(path, path_len, idx);
    if (ret < 0) {
        if (path[0] != '\0') vfs_unlink(path);
        spinlock_release(&unix_lock);
        return ret;
    }
    memcpy(ep->bound_path, path, (size_t)path_len);
    ep->bound_path[path_len] = '\0'; /* ensure safety */
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

    spinlock_acquire(&unix_lock);
    struct unix_ep *ep = ep_get(idx);
    if (!ep) { spinlock_release(&unix_lock); return -EBADF; }

    /* SOCK_DGRAM: just store the default destination */
    if (ep->type == SOCK_DGRAM) {
        int dst_len = (path[0] != '\0') ? (int)strlen(path) : plen;
        if (dst_len <= 0) { spinlock_release(&unix_lock); return -EDESTADDRREQ; }
        memcpy(ep->default_dst, path, (size_t)dst_len);
        ep->default_dst[dst_len] = '\0';
        ep->default_dst_len = dst_len;
        /* Verify the destination exists */
        struct path_entry *pe = path_lookup_len(path, dst_len);
        if (!pe) { spinlock_release(&unix_lock); return -ECONNREFUSED; }
        spinlock_release(&unix_lock);
        return 0;
    }

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

/* ════════════════════════════════════════════════════════════════════
 * SOCK_DGRAM support
 * ════════════════════════════════════════════════════════════════════ */

/* Enqueue a datagram to an endpoint's dgram queue.
 * Takes ownership of the data buffer (will kfree it after dequeue).
 * Caller must hold unix_lock. */
static int dgram_queue(struct unix_ep *ep, struct unix_dgram *dg)
{
    int next = (ep->dgram_head + 1) % UNIX_DGRAM_QUEUE;
    if (next == ep->dgram_tail)
        return -EAGAIN; /* queue full */

    ep->dgram_queue[ep->dgram_head] = *dg;
    ep->dgram_head = next;
    return 0;
}

/* Dequeue a datagram from an endpoint's dgram queue.
 * Caller must hold unix_lock.  The caller takes ownership of dg->data. */
static int dgram_dequeue(struct unix_ep *ep, struct unix_dgram *dg)
{
    if (ep->dgram_tail == ep->dgram_head)
        return -EAGAIN;

    *dg = ep->dgram_queue[ep->dgram_tail];
    ep->dgram_tail = (ep->dgram_tail + 1) % UNIX_DGRAM_QUEUE;
    return 0;
}

/* Send a datagram to a specific destination path.
 * Used by SOCK_DGRAM sendto() / sendmsg(). */
static int unix_dgram_sendto(int idx, const void *data, uint32_t len,
                             const struct sockaddr_un *dst_addr,
                             uint32_t addrlen, const struct msghdr *msg)
{
    if (!data && len > 0) return -EFAULT;
    if (len > UNIX_DGRAM_MAX) return -EMSGSIZE;

    char dst_path[UNIX_PATH_MAX];
    int dst_plen = (int)addrlen - (int)sizeof(uint16_t);
    if (dst_plen <= 0) return -EINVAL;
    if (dst_plen >= UNIX_PATH_MAX) dst_plen = UNIX_PATH_MAX - 1;
    memcpy(dst_path, dst_addr->sun_path, (size_t)dst_plen);
    int dst_len = (dst_path[0] != '\0') ? (int)strlen(dst_path) : dst_plen;
    if (dst_len <= 0) return -EDESTADDRREQ;

    spinlock_acquire(&unix_lock);

    struct unix_ep *src_ep = ep_get(idx);
    if (!src_ep || src_ep->type != SOCK_DGRAM) {
        spinlock_release(&unix_lock);
        return -EBADF;
    }

    /* Look up destination endpoint */
    struct path_entry *pe = path_lookup_len(dst_path, dst_len);
    if (!pe) {
        spinlock_release(&unix_lock);
        return -ECONNREFUSED;
    }
    struct unix_ep *dst_ep = ep_get(pe->ep_idx);
    if (!dst_ep || dst_ep->type != SOCK_DGRAM) {
        spinlock_release(&unix_lock);
        return -ECONNREFUSED;
    }

    /* Allocate and fill datagram */
    struct unix_dgram dg;
    memset(&dg, 0, sizeof(dg));
    dg.len = len;
    if (len > 0) {
        dg.data = (uint8_t *)kmalloc(len);
        if (!dg.data) {
            spinlock_release(&unix_lock);
            return -ENOMEM;
        }
        memcpy(dg.data, data, len);
    }

    /* Copy sender's bound path for peer address */
    if (src_ep->bound_path[0] != '\0' || src_ep->bound_path[1] != '\0') {
        int spl;
        if (src_ep->bound_path[0] == '\0') {
            for (spl = 1; spl < UNIX_PATH_MAX && src_ep->bound_path[spl] != '\0'; spl++)
                ;
        } else {
            spl = (int)strlen(src_ep->bound_path);
        }
        memcpy(dg.peer_path, src_ep->bound_path, (size_t)(spl < UNIX_PATH_MAX ? spl : UNIX_PATH_MAX - 1));
    }

    /* Process ancillary data from the message */
    if (msg && msg->msg_control && msg->msg_controllen > 0) {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
        while (cmsg) {
            if (cmsg->cmsg_level == SOL_SOCKET) {
                if (cmsg->cmsg_type == SCM_RIGHTS) {
                    /* Extract file descriptors */
                    int nfds = (int)(cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) / (int)sizeof(int);
                    if (nfds > 8) nfds = 8;
                    int *fds = (int *)CMSG_DATA(cmsg);
                    struct process *cur = process_get_current();
                    if (cur) {
                        for (int i = 0; i < nfds; i++) {
                            /* Duplicate the fd into the target's table */
                            dg.fds[dg.nfds++] = fds[i]; /* In a full impl we'd dup */
                        }
                    }
                } else if (cmsg->cmsg_type == SCM_CREDENTIALS) {
                    if (cmsg->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
                        struct ucred *uc = (struct ucred *)CMSG_DATA(cmsg);
                        dg.creds = *uc;
                        dg.has_creds = 1;
                    }
                }
            } else if (cmsg->cmsg_level == SOL_UNIX) {
                if (cmsg->cmsg_type == SCM_CREDENTIALS) {
                    if (cmsg->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
                        struct ucred *uc = (struct ucred *)CMSG_DATA(cmsg);
                        dg.creds = *uc;
                        dg.has_creds = 1;
                    }
                }
            }
            cmsg = CMSG_NXTHDR(msg, cmsg);
        }
    }

    /* If no credentials were passed explicitly, use current process creds */
    if (!dg.has_creds) {
        struct process *cur = process_get_current();
        if (cur) {
            dg.creds.pid = cur->pid;
            dg.creds.uid = cur->uid;
            dg.creds.gid = cur->gid;
            dg.has_creds = 1;
        }
    }

    int ret = dgram_queue(dst_ep, &dg);
    if (ret < 0) {
        if (dg.data) kfree(dg.data);
        spinlock_release(&unix_lock);
        return ret;
    }

    spinlock_release(&unix_lock);
    return (int)len;
}

/* Receive a datagram, extracting sender address and ancillary data.
 * Used by SOCK_DGRAM recvfrom() / recvmsg(). */
static int unix_dgram_recvfrom(int idx, void *data, uint32_t len,
                               struct sockaddr_un *src_addr,
                               uint32_t *addrlen, struct msghdr *msg)
{
    spinlock_acquire(&unix_lock);

    struct unix_ep *ep = ep_get(idx);
    if (!ep || ep->type != SOCK_DGRAM) {
        spinlock_release(&unix_lock);
        return -EBADF;
    }

    struct unix_dgram dg;
    int ret = dgram_dequeue(ep, &dg);
    if (ret < 0) {
        spinlock_release(&unix_lock);
        return -EAGAIN;
    }

    spinlock_release(&unix_lock);

    /* Copy data to caller */
    uint32_t copy_len = (len < dg.len) ? len : dg.len;
    if (data && copy_len > 0)
        memcpy(data, dg.data, copy_len);

    /* Fill source address if requested */
    if (src_addr && addrlen) {
        memset(src_addr, 0, sizeof(struct sockaddr_un));
        src_addr->sun_family = AF_UNIX;
        int spl = (int)strlen(dg.peer_path);
        if (spl > 0) {
            uint32_t slen = (uint32_t)(*addrlen - sizeof(uint16_t));
            if ((uint32_t)spl > slen) spl = (int)slen;
            memcpy(src_addr->sun_path, dg.peer_path, (size_t)spl);
        }
        *addrlen = sizeof(uint16_t) + (uint32_t)(dg.peer_path[0] ? (int)strlen(dg.peer_path) : 0);
    }

    /* Write ancillary data if msg_control was provided */
    if (msg && msg->msg_control && msg->msg_controllen > 0) {
        unsigned char *ctrl = (unsigned char *)msg->msg_control;
        uint64_t ctrl_len = msg->msg_controllen;
        uint64_t off = 0;

        /* SCM_CREDENTIALS */
        if (dg.has_creds && ctrl_len >= CMSG_SPACE(sizeof(struct ucred))) {
            struct cmsghdr *cm = (struct cmsghdr *)(ctrl + off);
            cm->cmsg_len   = CMSG_LEN(sizeof(struct ucred));
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type  = SCM_CREDENTIALS;
            memcpy(CMSG_DATA(cm), &dg.creds, sizeof(dg.creds));
            off += CMSG_SPACE(sizeof(struct ucred));
        }

        /* SCM_RIGHTS */
        if (dg.nfds > 0 && ctrl_len >= off + CMSG_SPACE((uint64_t)dg.nfds * sizeof(int))) {
            struct cmsghdr *cm = (struct cmsghdr *)(ctrl + off);
            cm->cmsg_len   = CMSG_LEN((uint64_t)dg.nfds * sizeof(int));
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type  = SCM_RIGHTS;
            memcpy(CMSG_DATA(cm), dg.fds, (size_t)dg.nfds * sizeof(int));
            off += CMSG_SPACE((uint64_t)dg.nfds * sizeof(int));
        }

        msg->msg_controllen = off;
    }

    /* Free the datagram payload */
    if (dg.data) kfree(dg.data);

    return (int)copy_len;
}

/* ════════════════════════════════════════════════════════════════════
 * sendmsg / recvmsg with ancillary data for SOCK_STREAM
 * ════════════════════════════════════════════════════════════════════ */

/* Process ancillary data from sendmsg and store it in the endpoint's
 * pending ancillary data fields for the receiver to pick up. */
static int unix_process_cmsg_ancillary(struct unix_ep *ep,
                                        const struct msghdr *msg)
{
    if (!msg || !msg->msg_control || msg->msg_controllen == 0)
        return 0;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
    while (cmsg) {
        if (cmsg->cmsg_level == SOL_SOCKET || cmsg->cmsg_level == SOL_UNIX) {
            if (cmsg->cmsg_type == SCM_CREDENTIALS &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
                struct ucred *uc = (struct ucred *)CMSG_DATA(cmsg);
                ep->pending_creds = *uc;
                ep->has_pending_creds = 1;
            } else if (cmsg->cmsg_type == SCM_RIGHTS) {
                int nfds = (int)(cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) / (int)sizeof(int);
                if (nfds > 8) nfds = 8;
                int *fds = (int *)CMSG_DATA(cmsg);
                for (int i = 0; i < nfds && ep->pending_nfds < 8; i++) {
                    ep->pending_fds[ep->pending_nfds++] = fds[i];
                }
            }
        }
        cmsg = CMSG_NXTHDR(msg, cmsg);
    }
    return 0;
}

/* If no credentials were sent explicitly, fill in the current process. */
static void unix_fill_default_creds(struct unix_ep *ep)
{
    if (!ep->has_pending_creds) {
        struct process *cur = process_get_current();
        if (cur) {
            ep->pending_creds.pid = cur->pid;
            ep->pending_creds.uid = cur->uid;
            ep->pending_creds.gid = cur->gid;
            ep->has_pending_creds = 1;
        }
    }
}

/* ── unix_sendmsg — send message with iovec and ancillary data ────── */
int unix_sendmsg(int idx, const struct msghdr *msg, int flags)
{
    (void)flags;
    if (!msg || msg->msg_iovlen < 1 || !msg->msg_iov)
        return -EINVAL;

    struct unix_ep *ep = ep_get(idx);
    if (!ep) return -EBADF;

    /* Handle SOCK_DGRAM: dispatch to datagram sender */
    if (ep->type == SOCK_DGRAM) {
        /* Determine destination address */
        const struct sockaddr_un *dst = NULL;
        uint32_t dst_len = 0;
        struct sockaddr_un tmp_dst;
        if (msg->msg_name && msg->msg_namelen >= sizeof(uint16_t)) {
            dst = (const struct sockaddr_un *)msg->msg_name;
            dst_len = msg->msg_namelen;
        } else if (ep->default_dst_len > 0) {
            /* Use connected default destination */
            memset(&tmp_dst, 0, sizeof(tmp_dst));
            tmp_dst.sun_family = AF_UNIX;
            memcpy(tmp_dst.sun_path, ep->default_dst, (size_t)ep->default_dst_len);
            dst = &tmp_dst;
            dst_len = sizeof(uint16_t) + (uint32_t)ep->default_dst_len;
        }
        /* If no explicit destination, use connected address (future) */
        if (!dst) return -EDESTADDRREQ;

        /* Gather data from iovec */
        uint32_t total = 0;
        for (uint32_t i = 0; i < msg->msg_iovlen; i++)
            total += (uint32_t)msg->msg_iov[i].iov_len;
        if (total > UNIX_DGRAM_MAX) return -EMSGSIZE;

        /* Allocate a temporary buffer and gather data */
        uint8_t *buf = (total > 0) ? (uint8_t *)kmalloc(total) : NULL;
        if (total > 0 && !buf) return -ENOMEM;
        uint32_t off = 0;
        for (uint32_t i = 0; i < msg->msg_iovlen; i++) {
            uint32_t clen = (uint32_t)msg->msg_iov[i].iov_len;
            if (clen > 0) {
                memcpy(buf + off, msg->msg_iov[i].iov_base, clen);
                off += clen;
            }
        }

        int ret = unix_dgram_sendto(idx, buf, total, dst, dst_len, msg);
        if (buf) kfree(buf);
        return ret;
    }

    /* SOCK_STREAM: write iovec data */
    uint64_t total = 0;
    for (uint32_t i = 0; i < msg->msg_iovlen; i++) {
        const void *data = msg->msg_iov[i].iov_base;
        uint64_t len = msg->msg_iov[i].iov_len;
        if (len == 0) continue;
        int sent = unix_send(idx, data, (uint32_t)(len > 65535 ? 65535 : len), 0);
        if (sent < 0) return total > 0 ? (int)total : sent;
        total += (uint64_t)sent;
    }

    /* Process ancillary data for stream sockets */
    unix_process_cmsg_ancillary(ep, msg);
    unix_fill_default_creds(ep);

    return (int)total;
}

/* ── unix_recvmsg — receive message with iovec and ancillary data ──── */
int unix_recvmsg(int idx, struct msghdr *msg, int flags)
{
    (void)flags;
    if (!msg || msg->msg_iovlen < 1 || !msg->msg_iov)
        return -EINVAL;

    struct unix_ep *ep = ep_get(idx);
    if (!ep) return -EBADF;

    /* Handle SOCK_DGRAM: dispatch to datagram receiver */
    if (ep->type == SOCK_DGRAM) {
        void *buf = msg->msg_iov[0].iov_base;
        uint64_t bufsize = msg->msg_iov[0].iov_len;
        struct sockaddr_un *src = (struct sockaddr_un *)msg->msg_name;
        uint32_t *addrlen = (msg->msg_name) ? &msg->msg_namelen : NULL;
        return unix_dgram_recvfrom(idx, buf, (uint32_t)(bufsize > UNIX_DGRAM_MAX ? UNIX_DGRAM_MAX : bufsize),
                                   src, addrlen, msg);
    }

    /* SOCK_STREAM: read iovec data */
    void *buf = msg->msg_iov[0].iov_base;
    uint64_t bufsize = msg->msg_iov[0].iov_len;
    int n = unix_recv(idx, buf, (uint32_t)(bufsize > 65535 ? 65535 : bufsize), 0);
    if (n <= 0) return -1;

    /* Provide ancillary data (credentials) if available */
    if (msg->msg_control && msg->msg_controllen > 0 && ep->has_pending_creds) {
        unsigned char *ctrl = (unsigned char *)msg->msg_control;
        uint64_t ctrl_len = msg->msg_controllen;

        /* SCM_CREDENTIALS */
        if (ctrl_len >= CMSG_SPACE(sizeof(struct ucred))) {
            struct cmsghdr *cm = (struct cmsghdr *)ctrl;
            cm->cmsg_len   = CMSG_LEN(sizeof(struct ucred));
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type  = SCM_CREDENTIALS;
            memcpy(CMSG_DATA(cm), &ep->pending_creds, sizeof(ep->pending_creds));
            msg->msg_controllen = CMSG_SPACE(sizeof(struct ucred));
        }

        /* SCM_RIGHTS */
        if (ep->pending_nfds > 0 &&
            msg->msg_controllen + CMSG_SPACE((uint64_t)ep->pending_nfds * sizeof(int)) <= ctrl_len) {
            struct cmsghdr *cm = (struct cmsghdr *)(ctrl + msg->msg_controllen);
            cm->cmsg_len   = CMSG_LEN((uint64_t)ep->pending_nfds * sizeof(int));
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type  = SCM_RIGHTS;
            memcpy(CMSG_DATA(cm), ep->pending_fds, (size_t)ep->pending_nfds * sizeof(int));
            msg->msg_controllen += CMSG_SPACE((uint64_t)ep->pending_nfds * sizeof(int));
        }

        /* Clear pending ancillary data */
        ep->has_pending_creds = 0;
        ep->pending_nfds = 0;
    } else {
        msg->msg_controllen = 0;
    }

    /* Fill in peer address for stream sockets */
    if (msg->msg_name && msg->msg_namelen >= sizeof(uint16_t)) {
        struct sockaddr_un *un = (struct sockaddr_un *)msg->msg_name;
        un->sun_family = AF_UNIX;
        un->sun_path[0] = '\0';
        msg->msg_namelen = sizeof(uint16_t);
    }

    return n;
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

    /* SOCK_DGRAM: readable if dgram queue has data, always writable */
    if (ep->type == SOCK_DGRAM) {
        rev |= POLLOUT;
        if (ep->dgram_tail != ep->dgram_head)
            rev |= POLLIN;
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

/* ── Socketpair — create a pair of connected AF_UNIX sockets ────────── *
 *
 * Returns two endpoint indices that are connected to each other via a
 * shared connection buffer.  The first endpoint behaves as the "server"
 * side, the second as the "client" side — both can send and receive.
 *
 * Returns 0 on success with *ep0 and *ep1 filled, or -1 on error. */

/* Forward declaration */
static int unix_socketpair_type(int *ep0, int *ep1, int type);

int unix_socketpair(int *ep0, int *ep1)
{
    return unix_socketpair_type(ep0, ep1, SOCK_STREAM);
}

static int unix_socketpair_type(int *ep0, int *ep1, int type)
{
    if (!ep0 || !ep1) return -EINVAL;

    spinlock_acquire(&unix_lock);

    int idx0 = ep_alloc();
    if (idx0 < 0) { spinlock_release(&unix_lock); return -ENOMEM; }

    int idx1 = ep_alloc();
    if (idx1 < 0) {
        ep_free(idx0);
        spinlock_release(&unix_lock);
        return -ENOMEM;
    }

    struct unix_conn *c = conn_alloc();
    if (!c) {
        ep_free(idx0);
        ep_free(idx1);
        spinlock_release(&unix_lock);
        return -ENOMEM;
    }

    /* Set up endpoint 0 as the "server" side */
    eps[idx0].type      = type;
    eps[idx0].state     = UNIX_CONNECTED;
    eps[idx0].conn      = (type == SOCK_STREAM) ? c : NULL;
    eps[idx0].is_server = 1;

    /* Set up endpoint 1 as the "client" side */
    eps[idx1].type      = type;
    eps[idx1].state     = UNIX_CONNECTED;
    eps[idx1].conn      = (type == SOCK_STREAM) ? c : NULL;
    eps[idx1].is_server = 0;

    /* For SOCK_DGRAM and SOCK_SEQPACKET, we don't use the conn object */
    if (type == SOCK_DGRAM || type == SOCK_SEQPACKET) {
        kfree(c->buf);
        kfree(c);
    }

    spinlock_release(&unix_lock);

    *ep0 = idx0;
    *ep1 = idx1;
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
#include "module.h"
module_init(af_unix_init);

/* ── Implement: af_unix_send ──────────────────────────── */
static int af_unix_send(void *sk, void *msg, size_t len)
{
    (void)sk; (void)msg; (void)len;
    kprintf("[af_unix] af_unix_send: use unix_send via socket layer\n");
    return -EOPNOTSUPP;
}
/* ── Implement: af_unix_recv ──────────────────────────── */
static int af_unix_recv(void *sk, void *buf, size_t len)
{
    (void)sk; (void)buf; (void)len;
    kprintf("[af_unix] af_unix_recv: use unix_recv via socket layer\n");
    return -EOPNOTSUPP;
}
/* ── Implement: af_unix_connect ───────────────────────── */
static int af_unix_connect(void *sk, void *addr, int addr_len)
{
    (void)sk; (void)addr; (void)addr_len;
    kprintf("[af_unix] af_unix_connect: use socket layer connect\n");
    return -EOPNOTSUPP;
}
/* ── Implement: af_unix_listen ────────────────────────── */
static int af_unix_listen(void *sk, int backlog)
{
    (void)sk; (void)backlog;
    kprintf("[af_unix] af_unix_listen: use socket layer listen\n");
    return -EOPNOTSUPP;
}
/* ── Implement: af_unix_accept ────────────────────────── */
static int af_unix_accept(void *sk, void *addr, void *addr_len)
{
    (void)sk; (void)addr; (void)addr_len;
    kprintf("[af_unix] af_unix_accept: use socket layer accept\n");
    return -EOPNOTSUPP;
}
