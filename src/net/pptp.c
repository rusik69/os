/*
 * pptp.c — PPTP (Point-to-Point Tunneling Protocol)
 *
 * Implements PPTP for VPN tunneling with GRE encapsulation.
 * PPTP uses a TCP control channel (port 1723) and GRE tunnel for data.
 *
 * This module provides the GRE encapsulation side and control
 * message handling for PPTP sessions.
 */

#define KERNEL_INTERNAL
#include "pptp.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"

#define PPTP_MAX_CALLS      64
#define PPTP_CTRL_PORT      1723
#define PPTP_GRE_PROTOCOL   0x880B

/* PPTP call state */
struct pptp_call {
    int      in_use;
    uint16_t call_id;
    uint16_t peer_call_id;
    uint32_t peer_ip;
    int      established;
    uint64_t rx_packets;
    uint64_t tx_packets;
};

static struct pptp_call g_calls[PPTP_MAX_CALLS];
static spinlock_t g_pptp_lock;
static int g_pptp_initialized = 0;

/* ── Call management ────────────────────────────────────────────────── */

int pptp_call_create(uint16_t call_id)
{
    spinlock_acquire(&g_pptp_lock);
    for (int i = 0; i < PPTP_MAX_CALLS; i++) {
        if (!g_calls[i].in_use) {
            struct pptp_call *c = &g_calls[i];
            memset(c, 0, sizeof(*c));
            c->in_use = 1;
            c->call_id = call_id;
            spinlock_release(&g_pptp_lock);
            kprintf("[PPTP] Created call %d (id=%u)\n", i, call_id);
            return i;
        }
    }
    spinlock_release(&g_pptp_lock);
    return -ENOSPC;
}

int pptp_call_connect(int call_idx, uint16_t peer_call_id, uint32_t peer_ip)
{
    spinlock_acquire(&g_pptp_lock);
    if (call_idx < 0 || call_idx >= PPTP_MAX_CALLS || !g_calls[call_idx].in_use) {
        spinlock_release(&g_pptp_lock);
        return -EINVAL;
    }
    struct pptp_call *c = &g_calls[call_idx];
    c->peer_call_id = peer_call_id;
    c->peer_ip = peer_ip;
    c->established = 1;
    spinlock_release(&g_pptp_lock);
    kprintf("[PPTP] Call %d connected to 0x%x peer_call=%u\n",
            call_idx, peer_ip, peer_call_id);
    return 0;
}

int pptp_call_disconnect(int call_idx)
{
    spinlock_acquire(&g_pptp_lock);
    if (call_idx < 0 || call_idx >= PPTP_MAX_CALLS || !g_calls[call_idx].in_use) {
        spinlock_release(&g_pptp_lock);
        return -EINVAL;
    }
    g_calls[call_idx].established = 0;
    spinlock_release(&g_pptp_lock);
    return 0;
}

int pptp_call_delete(int call_idx)
{
    spinlock_acquire(&g_pptp_lock);
    if (call_idx < 0 || call_idx >= PPTP_MAX_CALLS || !g_calls[call_idx].in_use) {
        spinlock_release(&g_pptp_lock);
        return -EINVAL;
    }
    memset(&g_calls[call_idx], 0, sizeof(struct pptp_call));
    spinlock_release(&g_pptp_lock);
    return 0;
}

/* ── GRE encapsulation (PPTP data) ─────────────────────────────────── */

int pptp_gre_encap(int call_idx, const void *payload, uint32_t payload_len,
                   void *out_buf, uint32_t out_len)
{
    if (payload_len + 8 > out_len) return -ENOSPC;

    spinlock_acquire(&g_pptp_lock);
    if (call_idx < 0 || call_idx >= PPTP_MAX_CALLS ||
        !g_calls[call_idx].in_use || !g_calls[call_idx].established) {
        spinlock_release(&g_pptp_lock);
        return -EINVAL;
    }
    struct pptp_call *c = &g_calls[call_idx];

    /* Build GRE header (minimal, enhanced for PPTP):
     *   C=1 (checksum present), R=0, K=1 (key present), S=0, s=0
     *   Recur=0, A=0, Flags=0, Ver=0
     *   Protocol = 0x880B (PPTP)
     *   Payload Length, Call ID, Sequence (optional)
     */
    uint8_t *p = (uint8_t *)out_buf;
    uint16_t gre_flags = 0x2001;  /* C=1, K=1, ver=1 (PPTP GRE) */
    uint16_t gre_proto = PPTP_GRE_PROTOCOL;

    p[0] = (uint8_t)(gre_flags >> 8);
    p[1] = (uint8_t)(gre_flags & 0xFF);
    p[2] = (uint8_t)(gre_proto >> 8);
    p[3] = (uint8_t)(gre_proto & 0xFF);
    p[4] = (uint8_t)((payload_len + 4) >> 8);  /* payload length */
    p[5] = (uint8_t)((payload_len + 4) & 0xFF);
    p[6] = (uint8_t)(c->call_id >> 8);
    p[7] = (uint8_t)(c->call_id & 0xFF);

    memcpy(p + 8, payload, payload_len);
    c->tx_packets++;
    spinlock_release(&g_pptp_lock);
    return (int)(8 + payload_len);
}

int pptp_gre_decap(const void *in_buf, uint32_t in_len,
                   void *payload, uint32_t *payload_len)
{
    if (in_len < 8) return -EINVAL;
    uint32_t plen = in_len - 8;
    if (payload && *payload_len >= plen)
        memcpy(payload, (const uint8_t *)in_buf + 8, plen);
    *payload_len = plen;
    return 0;
}

/* ── Initialization ─────────────────────────────────────────────────── */

void pptp_init(void)
{
    if (g_pptp_initialized) return;
    memset(g_calls, 0, sizeof(g_calls));
    spinlock_init(&g_pptp_lock);
    g_pptp_initialized = 1;
    kprintf("[OK] PPTP initialized (%d calls max)\n", PPTP_MAX_CALLS);
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Implement: pptp_xmit ────────────────── */
int pptp_xmit(void *skb, void *call)
{
    if (!skb || !call) {
        kprintf("[pptp] pptp_xmit: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[pptp] pptp_xmit: skb=%p call=%p (stub)\n", skb, call);
    return -EOPNOTSUPP;
}
/* ── Implement: pptp_rcv ────────────────── */
int pptp_rcv(void *skb)
{
    if (!skb) {
        kprintf("[pptp] pptp_rcv: NULL skb\n");
        return -EINVAL;
    }
    kprintf("[pptp] pptp_rcv: skb=%p (stub)\n", skb);
    return -EOPNOTSUPP;
}
/* ── Implement: pptp_connect ────────────────── */
int pptp_connect(struct pptp_conn *conn, uint32_t server_ip)
{
    if (!conn) {
        kprintf("[pptp] pptp_connect: NULL conn\n");
        return -EINVAL;
    }
    if (server_ip == 0) {
        kprintf("[pptp] pptp_connect: invalid server IP\n");
        return -EINVAL;
    }
    kprintf("[pptp] pptp_connect: conn=%p server_ip=%u.%u.%u.%u (stub)\n",
            (const void *)conn,
            (unsigned)((server_ip >> 24) & 0xFF), (unsigned)((server_ip >> 16) & 0xFF),
            (unsigned)((server_ip >> 8) & 0xFF), (unsigned)(server_ip & 0xFF));
    return -EOPNOTSUPP;
}
/* ── Implement: pptp_disconnect ────────────────── */
int pptp_disconnect(struct pptp_conn *conn)
{
    if (!conn) {
        kprintf("[pptp] pptp_disconnect: NULL conn\n");
        return -EINVAL;
    }
    kprintf("[pptp] pptp_disconnect: conn=%p (stub)\n", (const void *)conn);
    return -EOPNOTSUPP;
}
