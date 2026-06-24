/*
 * l2tp.c — L2TPv3 (Layer 2 Tunneling Protocol version 3)
 *
 * Implements L2TPv3 pseudowire encapsulation for Ethernet frames
 * over IP/UDP.  L2TPv3 provides a simpler, more efficient alternative
 * to L2TPv2 by removing the control channel and using a fixed-length
 * session header.
 *
 * Supported: L2TPv3 over IP (UDP encapsulation optional)
 */

#define KERNEL_INTERNAL
#include "l2tp.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"

#define L2TP_SESSION_MAX  64
#define L2TP_HDR_SIZE     12    /* L2TPv3 data header */

/* L2TPv3 session */
struct l2tp_session {
    int      in_use;
    uint32_t session_id;
    uint32_t peer_session_id;
    uint32_t tunnel_id;
    uint32_t peer_tunnel_id;
    int      udp_encap;         /* 1 = UDP, 0 = IP */
    uint16_t local_udp_port;
    uint16_t peer_udp_port;
    uint32_t peer_ip;
    uint64_t rx_packets;
    uint64_t tx_packets;
    int      cookie_len;        /* 0, 4, or 8 */
    uint64_t local_cookie;
    uint64_t peer_cookie;
};

static struct l2tp_session g_sessions[L2TP_SESSION_MAX];
static spinlock_t g_l2tp_lock;
static int g_l2tp_initialized = 0;

/* ── Session management ─────────────────────────────────────────────── */

int l2tp_session_create(uint32_t session_id, uint32_t tunnel_id)
{
    spinlock_acquire(&g_l2tp_lock);
    for (int i = 0; i < L2TP_SESSION_MAX; i++) {
        if (!g_sessions[i].in_use) {
            struct l2tp_session *s = &g_sessions[i];
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            s->session_id = session_id;
            s->tunnel_id = tunnel_id;
            spinlock_release(&g_l2tp_lock);
            kprintf("[L2TP] Created session %d (sid=%u tid=%u)\n",
                    i, session_id, tunnel_id);
            return i;
        }
    }
    spinlock_release(&g_l2tp_lock);
    return -ENOSPC;
}

int l2tp_session_delete(int session_id)
{
    spinlock_acquire(&g_l2tp_lock);
    if (session_id < 0 || session_id >= L2TP_SESSION_MAX ||
        !g_sessions[session_id].in_use) {
        spinlock_release(&g_l2tp_lock);
        return -EINVAL;
    }
    memset(&g_sessions[session_id], 0, sizeof(struct l2tp_session));
    spinlock_release(&g_l2tp_lock);
    return 0;
}

int l2tp_session_set_peer(int session_id, uint32_t peer_sid,
                           uint32_t peer_tid, uint32_t peer_ip)
{
    spinlock_acquire(&g_l2tp_lock);
    if (session_id < 0 || session_id >= L2TP_SESSION_MAX ||
        !g_sessions[session_id].in_use) {
        spinlock_release(&g_l2tp_lock);
        return -EINVAL;
    }
    struct l2tp_session *s = &g_sessions[session_id];
    s->peer_session_id = peer_sid;
    s->peer_tunnel_id = peer_tid;
    s->peer_ip = peer_ip;
    spinlock_release(&g_l2tp_lock);
    return 0;
}

int l2tp_session_set_cookie(int session_id, uint64_t cookie, int len)
{
    if (len != 0 && len != 4 && len != 8) return -EINVAL;
    spinlock_acquire(&g_l2tp_lock);
    if (session_id < 0 || session_id >= L2TP_SESSION_MAX ||
        !g_sessions[session_id].in_use) {
        spinlock_release(&g_l2tp_lock);
        return -EINVAL;
    }
    struct l2tp_session *s = &g_sessions[session_id];
    s->local_cookie = cookie;
    s->cookie_len = len;
    spinlock_release(&g_l2tp_lock);
    return 0;
}

/* ── Encapsulation ───────────────────────────────────────────────────── */

int l2tp_encap(int session_id, const void *payload, uint32_t payload_len,
               void *out_buf, uint32_t out_len)
{
    if (payload_len + L2TP_HDR_SIZE > out_len) return -ENOSPC;
    spinlock_acquire(&g_l2tp_lock);
    if (session_id < 0 || session_id >= L2TP_SESSION_MAX ||
        !g_sessions[session_id].in_use) {
        spinlock_release(&g_l2tp_lock);
        return -EINVAL;
    }
    struct l2tp_session *s = &g_sessions[session_id];

    /* Build L2TPv3 data header:
     *   Bits 0-3:  ver = 3
     *   Bit  4:    T (type) = 0 (data)
     *   Bit  7:    S (sequence) = 0
     *   Bits 8-31: session_id
     */
    uint8_t *p = (uint8_t *)out_buf;
    p[0] = 0x30;           /* ver=3, T=0, L=0, S=0 */
    p[1] = 0x00;
    p[2] = (uint8_t)(s->session_id >> 24);
    p[3] = (uint8_t)(s->session_id >> 16);
    p[4] = (uint8_t)(s->session_id >> 8);
    p[5] = (uint8_t)(s->session_id);
    p[6] = (uint8_t)(s->cookie_len > 4 ? (s->local_cookie >> 56) : 0);
    p[7] = (uint8_t)(s->cookie_len > 4 ? (s->local_cookie >> 48) : 0);
    p[8] = (uint8_t)(s->cookie_len > 0 ? (s->local_cookie >> 40) : 0);
    p[9] = (uint8_t)(s->cookie_len > 0 ? (s->local_cookie >> 32) : 0);
    p[10] = (uint8_t)(s->local_cookie >> 24);
    p[11] = (uint8_t)(s->local_cookie >> 16);
    /* Cookie continues... we just use first 8 bytes for simplicity */

    memcpy(p + L2TP_HDR_SIZE, payload, payload_len);
    s->tx_packets++;
    spinlock_release(&g_l2tp_lock);
    return (int)(L2TP_HDR_SIZE + payload_len);
}

int l2tp_decap(const void *in_buf, uint32_t in_len,
               void *payload, uint32_t *payload_len)
{
    if (in_len < L2TP_HDR_SIZE) return -EINVAL;
    uint32_t plen = in_len - L2TP_HDR_SIZE;
    if (payload && *payload_len >= plen) {
        memcpy(payload, (const uint8_t *)in_buf + L2TP_HDR_SIZE, plen);
    }
    *payload_len = plen;
    return 0;
}

/* ── Initialization ─────────────────────────────────────────────────── */

void l2tp_init(void)
{
    if (g_l2tp_initialized) return;
    memset(g_sessions, 0, sizeof(g_sessions));
    spinlock_init(&g_l2tp_lock);
    g_l2tp_initialized = 1;
    kprintf("[OK] L2TPv3 initialized (%d sessions max)\n", L2TP_SESSION_MAX);
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Implement: l2tp_xmit ────────────────── */
int l2tp_xmit(void *skb, void *session)
{
    if (!skb || !session) {
        kprintf("[l2tp] l2tp_xmit: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[l2tp] l2tp_xmit: skb=%p session=%p (stub)\n", skb, session);
    return -EOPNOTSUPP;
}
/* ── Implement: l2tp_rcv ────────────────── */
int l2tp_rcv(void *skb)
{
    if (!skb) {
        kprintf("[l2tp] l2tp_rcv: NULL skb\n");
        return -EINVAL;
    }
    kprintf("[l2tp] l2tp_rcv: skb=%p (stub)\n", skb);
    return -EOPNOTSUPP;
}
/* ── Stub: l2tp_session_create ─────────────────────── */
struct l2tp_session *l2tp_session_create(uint32_t session_id, uint32_t peer_session_id)
{
    (void)session_id;
    (void)peer_session_id;
    kprintf("[l2tp] l2tp_session_create: not yet implemented\n");
    return NULL;
}
/* ── Stub: l2tp_tunnel_create ──────────────────────── */
struct l2tp_tunnel *l2tp_tunnel_create(uint32_t tunnel_id, uint32_t peer_tunnel_id)
{
    (void)tunnel_id;
    (void)peer_tunnel_id;
    kprintf("[L2TP] l2tp_tunnel_create: not yet implemented\n");
    return -EOPNOTSUPP;
}
