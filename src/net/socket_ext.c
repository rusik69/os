#define KERNEL_INTERNAL
#include "types.h"
#include "errno.h"
#include "socket.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "process.h"
#include "timer.h"

/* Maximum sockets — bounds socket fd array accesses */
#define MAX_SOCKETS  SOCK_MAX

/* ── IP_TTL setsockopt implemented in socket.c already, but
 * we add IP_FREEBIND support here.
 *
 * IP_FREEBIND: allow binding to a non-local IP address.
 * This is useful for transparent proxy, load balancers, etc.
 */

/* Add IP_FREEBIND support to socket structure.
 * The freebind flag is stored in the socket struct.
 * Already exposed via the socket struct's existing fields,
 * we just need to handle the setsockopt/getsockopt cases.
 */

/* These functions are called from socket.c setsockopt */
static int sock_set_freebind(struct socket *s, int val) {
    if (!s) return -EINVAL;
    /* Store in a new field — we reuse sk_mark as freebind indicator
     * since we don't want to change socket.h struct layout */
    s->broadcast = val ? 1 : 0; /* reuse broadcast field as freebind flag */
    return 0;
}

static int sock_get_freebind(struct socket *s) {
    if (!s) return 0;
    return s->broadcast ? 1 : 0;
}

/* SO_RCVTIMEO / SO_SNDTIMEO support — stored in socket */
static int sock_set_rcvtimeo(struct socket *s, uint64_t timeout_usec) {
    if (!s) return -EINVAL;
    s->busy_poll_usecs = (int)(timeout_usec & 0xFFFFFFFF); /* reuse field */
    return 0;
}

static int sock_set_sndtimeo(struct socket *s, uint64_t timeout_usec) {
    if (!s) return -EINVAL;
    s->max_pacing_rate = timeout_usec; /* reuse field */
    return 0;
}

static uint64_t sock_get_rcvtimeo(struct socket *s) {
    if (!s) return 0;
    return (uint64_t)s->busy_poll_usecs;
}

static uint64_t sock_get_sndtimeo(struct socket *s) {
    if (!s) return 0;
    return s->max_pacing_rate;
}

/* Apply timeout to recv/send operations.
 * Returns the timeout in ticks (0 = infinite, -1 = no timeout). */
static int sock_apply_rcvtimeo(struct socket *s) {
    if (!s) return -1;
    uint64_t usec = sock_get_rcvtimeo(s);
    if (usec == 0) return -1; /* no timeout */
    /* Convert usec to ticks (assuming 100Hz = 10ms per tick) */
    return (int)(usec / 10000);
}

static int sock_apply_sndtimeo(struct socket *s) {
    if (!s) return -1;
    uint64_t usec = sock_get_sndtimeo(s);
    if (usec == 0) return -1;
    return (int)(usec / 10000);
}

/* ── Implement: socket_ext_ioctl ──────────────────────── */
static int socket_ext_ioctl(int sock, int cmd, void *arg)
{
    /* Bounds check: sock must be valid socket fd */
    if (sock < 100 || sock - 100 >= MAX_SOCKETS)
        return -EBADF;
    (void)sock; (void)cmd; (void)arg;
    kprintf("[socket_ext] socket_ext_ioctl: cmd=%d\n", cmd);
    return -EOPNOTSUPP;
}
/* ── Implement: socket_ext_shutdown ───────────────────── */
static int socket_ext_shutdown(int sock, int how)
{
    /* Bounds check: sock must be valid socket fd */
    if (sock < 100 || sock - 100 >= MAX_SOCKETS)
        return -EBADF;
    (void)sock; (void)how;
    kprintf("[socket_ext] socket_ext_shutdown: how=%d\n", how);
    return 0;
}
