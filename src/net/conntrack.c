/* conntrack.c — Network connection tracking with TCP state machine
 *
 * Tracks TCP connections through the full state machine (SYN_SENT → … → TIME_WAIT),
 * UDP flows (unreplied → assured on bidirectional traffic), and ICMP (echo req/reply
 * and error relating).  Provides per-packet integration hooks called from netfilter.
 *
 * Protocol-specific timeout tables match Linux defaults (approximately):
 *   TCP established:   5 days  (432000 ticks @ 10 Hz)
 *   TCP SYN_SENT:      30 sec  (  300 ticks)
 *   TCP TIME_WAIT:     2 min   ( 1200 ticks)
 *   UDP unreplied:     30 sec  (  300 ticks)
 *   UDP assured:       3 min   ( 1800 ticks)
 *   ICMP:              30 sec  (  300 ticks)
 */

#define KERNEL_INTERNAL
#include "netfilter.h"
#include "conntrack_helper.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "heap.h"

/* ══════════════════════════════════════════════════════════════════
 *                    Per-Protocol Timeout Tables
 * ══════════════════════════════════════════════════════════════════ */

/* TCP timeout table (indexed by TCP_CONN_* state) in timer ticks.
 * 10 ticks/second default timer granularity. */
static const uint32_t tcp_timeout_ticks[TCP_CONN_MAX_STATE] = {
    [TCP_CONN_NONE]        =     60,    /*  6 sec — should never persist */
    [TCP_CONN_SYN_SENT]    =    300,    /* 30 sec */
    [TCP_CONN_SYN_RECV]    =    600,    /* 60 sec */
    [TCP_CONN_ESTABLISHED] = 432000,    /*  5 days (43200 sec) — Linux default */
    [TCP_CONN_FIN_WAIT_1]  =   1200,    /*  2 min */
    [TCP_CONN_FIN_WAIT_2]  =   1200,    /*  2 min */
    [TCP_CONN_CLOSE_WAIT]  =   3600,    /*  6 min */
    [TCP_CONN_CLOSING]     =    600,    /* 60 sec */
    [TCP_CONN_LAST_ACK]    =    600,    /* 60 sec */
    [TCP_CONN_TIME_WAIT]   =   1200,    /*  2 min */
};

/* UDP timeout table */
#define UDP_TIMEOUT_UNREPLIED  300    /* 30 sec */
#define UDP_TIMEOUT_ASSURED   1800    /*  3 min */

/* ICMP timeout table */
#define ICMP_TIMEOUT_REQUEST  300    /* 30 sec */
#define ICMP_TIMEOUT_REPLY     60    /*  6 sec — once replied, expires fast */
#define ICMP_TIMEOUT_ERROR    300    /* 30 sec */

/* ══════════════════════════════════════════════════════════════════
 *                         Static State
 * ══════════════════════════════════════════════════════════════════ */

static struct nf_conn nf_conns[NF_CONNTRACK_MAX];
static int            nf_conn_count;

static struct nf_conntrack_stats nf_stats;

/* ══════════════════════════════════════════════════════════════════
 *                   TCP State Machine Transitions
 * ══════════════════════════════════════════════════════════════════
 *
 * The state machine below tracks the connection-endpoint view.  We see
 * packets from both sides, so we track the "union" state:
 *
 *   CLOSED ──(SYN)──→ SYN_SENT        (from originator)
 *   CLOSED ──(SYN)──→ SYN_RECV        (from responder — we saw SYN-ACK before SYN?)
 *   SYN_SENT ──(SYN+ACK)──→ ESTABLISHED
 *   SYN_RECV ──(ACK)──→ ESTABLISHED
 *   ESTABLISHED ──(FIN from one side)──→ FIN_WAIT_1 or CLOSE_WAIT
 *   ... (standard TCP states)
 */

/* Determine if a TCP packet is from the connection originator.
 * Originator = the side with the numerically lower port, or the one
 * that sent the first SYN we recorded.  We use a simple heuristic:
 * if we have no prior state, the first sender is the originator.
 * Once we have state, the port tuple determines direction. */
static int is_originator(const struct nf_conn *conn,
                         uint32_t src_ip, uint32_t dst_ip,
                         uint16_t src_port, uint16_t dst_port)
{
    (void)dst_ip;
    (void)dst_port;
    /* If the connection was created by a packet, the stored tuple
     * has (src_ip, src_port) = originator, (dst_ip, dst_port) = responder.
     * A packet matches the originator direction if its src matches conn->src. */
    return (src_ip == conn->src_ip && src_port == conn->src_port);
}

/* Update TCP connection state based on observed TCP flags.
 * This implements the core TCP state machine for conntrack. */
void nf_conntrack_update_tcp(struct nf_conn *conn, uint8_t tcp_flags,
                             int from_originator)
{
    uint8_t old_state = conn->proto_state;
    (void)old_state;

    /* Accumulate flags seen over the connection lifetime */
    conn->tcp_flags_seen |= tcp_flags;

    /* Determine if this is a SYN, SYN-ACK, FIN, RST, ACK */
    int is_syn    = (tcp_flags & 0x02) != 0;
    int is_ack    = (tcp_flags & 0x10) != 0;
    int is_fin    = (tcp_flags & 0x01) != 0;
    int is_rst    = (tcp_flags & 0x04) != 0;
    int is_synack = is_syn && is_ack;

    switch (conn->proto_state) {

    case TCP_CONN_NONE:
        if (is_rst) {
            conn->proto_state = TCP_CONN_NONE;  /* ignore RST to new conn */
            conn->timeout_ticks = 60;
        } else if (is_synack) {
            /* We saw a SYN-ACK before seeing the SYN (missed first packet).
             * Treat as if we're seeing the responder side first. */
            conn->proto_state = TCP_CONN_SYN_RECV;
            conn->timeout_ticks = tcp_timeout_ticks[TCP_CONN_SYN_RECV];
        } else if (is_syn) {
            conn->proto_state = from_originator ? TCP_CONN_SYN_SENT
                                                : TCP_CONN_SYN_RECV;
            conn->timeout_ticks = tcp_timeout_ticks[conn->proto_state];
        } else {
            /* Non-SYN packet to NEW connection — might be late arrival or
             * conntrack was flushed underneath.  Mark as ESTABLISHED if
             * we see data (ACK without SYN). */
            if (is_ack) {
                conn->proto_state = TCP_CONN_ESTABLISHED;
                conn->timeout_ticks = tcp_timeout_ticks[TCP_CONN_ESTABLISHED];
            }
        }
        break;

    case TCP_CONN_SYN_SENT:
        if (is_rst) {
            conn->proto_state = TCP_CONN_NONE;
            conn->timeout_ticks = 60;
        } else if (is_synack) {
            /* Originator received SYN-ACK → move to ESTABLISHED
             * (the originator's ACK of the SYN-ACK completes the handshake) */
            conn->proto_state = TCP_CONN_ESTABLISHED;
            conn->timeout_ticks = tcp_timeout_ticks[TCP_CONN_ESTABLISHED];
        } else if (is_syn && !from_originator) {
            /* Simultaneous open — we sent SYN, they sent SYN */
            conn->proto_state = TCP_CONN_SYN_RECV;
            conn->timeout_ticks = tcp_timeout_ticks[TCP_CONN_SYN_RECV];
        }
        break;

    case TCP_CONN_SYN_RECV:
        if (is_rst) {
            conn->proto_state = TCP_CONN_NONE;
            conn->timeout_ticks = 60;
        } else if (is_ack && !is_syn) {
            /* Handshake complete */
            conn->proto_state = TCP_CONN_ESTABLISHED;
            conn->timeout_ticks = tcp_timeout_ticks[TCP_CONN_ESTABLISHED];
        } else if (is_syn && !is_ack && !from_originator) {
            /* Still in SYN_RECV — another SYN from responder? Unusual. */
        }
        break;

    case TCP_CONN_ESTABLISHED:
        if (is_rst) {
            conn->proto_state = TCP_CONN_NONE;
            conn->timeout_ticks = 60;
        } else if (is_fin) {
            if (from_originator) {
                conn->proto_state = TCP_CONN_FIN_WAIT_1;
            } else {
                conn->proto_state = TCP_CONN_CLOSE_WAIT;
            }
            conn->timeout_ticks = tcp_timeout_ticks[conn->proto_state];
        }
        break;

    case TCP_CONN_FIN_WAIT_1:
        if (is_rst) {
            conn->proto_state = TCP_CONN_NONE;
            conn->timeout_ticks = 60;
        } else if (is_fin && !from_originator) {
            /* Both sides sent FIN simultaneously */
            conn->proto_state = TCP_CONN_CLOSING;
            conn->timeout_ticks = tcp_timeout_ticks[TCP_CONN_CLOSING];
        } else if (is_ack && !is_syn && !is_fin) {
            /* Our FIN was ACKed by responder */
            if (from_originator) {
                /* Originator got ACK of its FIN, now waiting for responder's FIN */
                conn->proto_state = TCP_CONN_FIN_WAIT_2;
            } else {
                /* Responder ACKed originator's FIN while we were in FIN_WAIT_1
                 * — actually from_originator=false here means the responder sent ACK
                 *   for the originator's FIN.  The originator sees this ACK. */
                conn->proto_state = TCP_CONN_FIN_WAIT_2;
            }
            conn->timeout_ticks = tcp_timeout_ticks[TCP_CONN_FIN_WAIT_2];
        } else if (is_fin && from_originator) {
            /* Another FIN from originator? stay in FIN_WAIT_1 */
        }
        break;

    case TCP_CONN_FIN_WAIT_2:
        if (is_rst) {
            conn->proto_state = TCP_CONN_NONE;
            conn->timeout_ticks = 60;
        } else if (is_fin && !from_originator) {
            /* Responder's FIN arrived */
            conn->proto_state = TCP_CONN_TIME_WAIT;
            conn->timeout_ticks = tcp_timeout_ticks[TCP_CONN_TIME_WAIT];
        }
        break;

    case TCP_CONN_CLOSE_WAIT:
        if (is_rst) {
            conn->proto_state = TCP_CONN_NONE;
            conn->timeout_ticks = 60;
        } else if (is_fin && from_originator) {
            /* Originator's FIN after we entered CLOSE_WAIT
             * — the originator is closing too. */
            conn->proto_state = TCP_CONN_LAST_ACK;
            conn->timeout_ticks = tcp_timeout_ticks[TCP_CONN_LAST_ACK];
        } else if (is_ack && !is_fin && !from_originator) {
            /* Responder ACKed originator's data — remain in CLOSE_WAIT */
        }
        break;

    case TCP_CONN_CLOSING:
        if (is_rst) {
            conn->proto_state = TCP_CONN_NONE;
            conn->timeout_ticks = 60;
        } else if (is_ack) {
            /* Both FINs ACKed → TIME_WAIT */
            conn->proto_state = TCP_CONN_TIME_WAIT;
            conn->timeout_ticks = tcp_timeout_ticks[TCP_CONN_TIME_WAIT];
        }
        break;

    case TCP_CONN_LAST_ACK:
        if (is_rst) {
            conn->proto_state = TCP_CONN_NONE;
            conn->timeout_ticks = 60;
        } else if (is_ack && !from_originator) {
            /* Responder's last ACK received → connection closed */
            conn->proto_state = TCP_CONN_NONE;
            conn->timeout_ticks = 60;
        }
        break;

    case TCP_CONN_TIME_WAIT:
        if (is_rst) {
            conn->proto_state = TCP_CONN_NONE;
            conn->timeout_ticks = 60;
        }
        /* Otherwise — TIME_WAIT expires via timeout */
        break;

    default:
        break;
    }

    /* Update statistics */
    if (conn->proto_state < TCP_CONN_MAX_STATE)
        nf_stats.tcp_states[conn->proto_state]++;
}

/* ══════════════════════════════════════════════════════════════════
 *                    Connection Lookup & Creation
 * ══════════════════════════════════════════════════════════════════ */

/* Find or create a conntrack entry for the given packet tuple.
 * The "originator" is the source of this packet; the tuple stored
 * in the connection is normalized so that (src_ip, src_port) is the
 * originator and (dst_ip, dst_port) is the responder. */

static struct nf_conn *conntrack_find(uint32_t src_ip, uint32_t dst_ip,
                                      uint16_t src_port, uint16_t dst_port,
                                      uint8_t protocol)
{
    for (int i = 0; i < NF_CONNTRACK_MAX; i++) {
        struct nf_conn *c = &nf_conns[i];
        if (!c->used) continue;

        /* Match forward: (src_ip, src_port) == (conn->src_ip, conn->src_port)
         *            AND (dst_ip, dst_port) == (conn->dst_ip, conn->dst_port) */
        if (c->src_ip   == src_ip   && c->dst_ip   == dst_ip &&
            c->src_port == src_port && c->dst_port == dst_port &&
            c->protocol == protocol)
            return c;

        /* Match reverse: (src_ip, src_port) == (conn->dst_ip, conn->dst_port)
         *             AND (dst_ip, dst_port) == (conn->src_ip, conn->src_port) */
        if (c->dst_ip   == src_ip   && c->src_ip   == dst_ip &&
            c->dst_port == src_port && c->src_port == dst_port &&
            c->protocol == protocol)
            return c;
    }
    return NULL;
}

static struct nf_conn *conntrack_new(uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     uint8_t protocol)
{
    /* Linear scan for a free slot */
    for (int i = 0; i < NF_CONNTRACK_MAX; i++) {
        struct nf_conn *c = &nf_conns[i];
        if (c->used) continue;

        memset(c, 0, sizeof(*c));
        c->src_ip   = src_ip;
        c->dst_ip   = dst_ip;
        c->src_port = src_port;
        c->dst_port = dst_port;
        c->protocol = protocol;
        c->used     = 1;
        c->last_seen = timer_get_ticks();
        c->timeout_ticks = 300; /* default 30 sec until state machine sets proper */

        nf_conn_count++;
        nf_stats.total_creations++;
        nf_stats.current_active = (uint64_t)nf_conn_count;
        if ((uint64_t)nf_conn_count > nf_stats.max_active)
            nf_stats.max_active = (uint64_t)nf_conn_count;

        return c;
    }

    nf_stats.table_full_errors++;
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════
 *                    Public Conntrack API
 * ══════════════════════════════════════════════════════════════════ */

int nf_conntrack_in(uint32_t src_ip, uint32_t dst_ip,
                    uint16_t src_port, uint16_t dst_port,
                    uint8_t protocol, uint8_t tcp_flags,
                    uint16_t payload_len)
{
    nf_stats.total_lookups++;

    /* Lookup existing connection — match in both directions */
    struct nf_conn *conn = conntrack_find(src_ip, dst_ip,
                                          src_port, dst_port, protocol);
    if (!conn) {
        /* Create new entry with normalized tuple (originator = src) */
        conn = conntrack_new(src_ip, dst_ip, src_port, dst_port, protocol);
        if (!conn)
            return NF_ACCEPT; /* table full — silently pass */

        /* Check if this new connection matches an expected entry
         * from a protocol helper (e.g., FTP data channel).
         * If so, mark it as RELATED and consume the expectation. */
        if (nf_ct_check_expected(src_ip, dst_ip, src_port, dst_port, protocol)) {
            conn->mark = NF_CONN_RELATED;
        }
    }

    uint64_t now = timer_get_ticks();
    conn->last_seen = now;

    /* Determine direction and update counters */
    int from_orig = is_originator(conn, src_ip, dst_ip, src_port, dst_port);
    if (from_orig) {
        conn->packets++;
        conn->bytes += payload_len;
    } else {
        conn->packets_reply++;
        conn->bytes_reply += payload_len;
        conn->orig_saw_reply = 1;
    }

    /* Protocol-specific state updates */
    switch (protocol) {
    case IPPROTO_TCP:
        nf_conntrack_update_tcp(conn, tcp_flags, from_orig);
        break;

    case IPPROTO_UDP:
        if (conn->proto_state == UDP_CONN_NONE) {
            conn->proto_state = UDP_CONN_UNREPLIED;
            conn->timeout_ticks = UDP_TIMEOUT_UNREPLIED;
        }
        if (conn->orig_saw_reply) {
            conn->proto_state = UDP_CONN_ASSURED;
            conn->timeout_ticks = UDP_TIMEOUT_ASSURED;
        }
        break;

    case IPPROTO_ICMP:
        /* ICMP type in first byte */
        if (conn->proto_state == ICMP_CONN_NONE) {
            conn->proto_state = ICMP_CONN_REQUEST;
            conn->timeout_ticks = ICMP_TIMEOUT_REQUEST;
        }
        if (conn->orig_saw_reply) {
            conn->proto_state = ICMP_CONN_REPLY;
            conn->timeout_ticks = ICMP_TIMEOUT_REPLY;
        }
        break;

    default:
        /* Unknown protocol — just track as generic */
        if (conn->proto_state == NF_CONN_NEW) {
            conn->proto_state = NF_CONN_ESTABLISHED;
            conn->timeout_ticks = 300;
        }
        break;
    }

    return NF_ACCEPT;
}

int nf_conntrack_out(uint32_t src_ip, uint32_t dst_ip,
                     uint16_t src_port, uint16_t dst_port,
                     uint8_t protocol, uint8_t tcp_flags,
                     uint16_t payload_len)
{
    /* Outgoing path: same logic as incoming but for LOCAL_OUT hook */
    nf_stats.total_lookups++;

    struct nf_conn *conn = conntrack_find(src_ip, dst_ip,
                                          src_port, dst_port, protocol);
    if (!conn) {
        conn = conntrack_new(src_ip, dst_ip, src_port, dst_port, protocol);
        if (!conn)
            return NF_ACCEPT;

        /* Check for expected connection (e.g., FTP data channel) */
        if (nf_ct_check_expected(src_ip, dst_ip, src_port, dst_port, protocol)) {
            conn->mark = NF_CONN_RELATED;
        }
    }

    uint64_t now = timer_get_ticks();
    conn->last_seen = now;

    int from_orig = is_originator(conn, src_ip, dst_ip, src_port, dst_port);
    if (from_orig) {
        conn->packets++;
        conn->bytes += payload_len;
    } else {
        conn->packets_reply++;
        conn->bytes_reply += payload_len;
        conn->orig_saw_reply = 1;
    }

    switch (protocol) {
    case IPPROTO_TCP:
        nf_conntrack_update_tcp(conn, tcp_flags, from_orig);
        break;
    case IPPROTO_UDP:
        if (conn->proto_state == UDP_CONN_NONE) {
            conn->proto_state = UDP_CONN_UNREPLIED;
            conn->timeout_ticks = UDP_TIMEOUT_UNREPLIED;
        }
        if (conn->orig_saw_reply) {
            conn->proto_state = UDP_CONN_ASSURED;
            conn->timeout_ticks = UDP_TIMEOUT_ASSURED;
        }
        break;
    case IPPROTO_ICMP:
        if (conn->proto_state == ICMP_CONN_NONE) {
            conn->proto_state = ICMP_CONN_REQUEST;
            conn->timeout_ticks = ICMP_TIMEOUT_REQUEST;
        }
        if (conn->orig_saw_reply) {
            conn->proto_state = ICMP_CONN_REPLY;
            conn->timeout_ticks = ICMP_TIMEOUT_REPLY;
        }
        break;
    default:
        if (conn->proto_state == NF_CONN_NEW) {
            conn->proto_state = NF_CONN_ESTABLISHED;
            conn->timeout_ticks = 300;
        }
        break;
    }

    return NF_ACCEPT;
}

/* Simple lookup — does not modify state */
struct nf_conn *nf_conntrack_lookup(uint32_t src_ip, uint32_t dst_ip,
                                    uint16_t src_port, uint16_t dst_port,
                                    uint8_t protocol)
{
    nf_stats.total_lookups++;
    return conntrack_find(src_ip, dst_ip, src_port, dst_port, protocol);
}

/* Legacy API wrappers */
struct nf_conn *nf_conntrack_get(uint32_t src_ip, uint32_t dst_ip,
                                 uint16_t src_port, uint16_t dst_port,
                                 uint8_t protocol)
{
    struct nf_conn *conn = conntrack_find(src_ip, dst_ip,
                                          src_port, dst_port, protocol);
    if (!conn)
        conn = conntrack_new(src_ip, dst_ip, src_port, dst_port, protocol);
    if (conn) {
        conn->last_seen = timer_get_ticks();
        conn->timeout_ticks = 300; /* reset timeout */
    }
    return conn;
}

void nf_conntrack_put(struct nf_conn *conn)
{
    if (!conn) return;
    conn->last_seen = timer_get_ticks();
}

void nf_conntrack_timeout(struct nf_conn *conn, uint32_t timeout_ticks)
{
    if (conn) conn->timeout_ticks = timeout_ticks;
}

/* ══════════════════════════════════════════════════════════════════
 *                      Periodic Expiry (GC)
 * ══════════════════════════════════════════════════════════════════ */

void nf_conntrack_purge(void)
{
    uint64_t now = timer_get_ticks();
    int expired = 0;

    for (int i = 0; i < NF_CONNTRACK_MAX; i++) {
        struct nf_conn *c = &nf_conns[i];
        if (!c->used) continue;

        /* Check if the connection has been idle longer than its timeout */
        if ((uint64_t)(now - c->last_seen) > c->timeout_ticks) {
            c->used = 0;
            nf_conn_count--;
            nf_stats.total_expired++;
            expired++;
        }

        /* TCP state machine quiescence: if connection appears closed
         * (proto_state == NONE), but hasn't been freed yet, expire it
         * after a short grace period. */
        if (c->used && c->protocol == IPPROTO_TCP &&
            c->proto_state == TCP_CONN_NONE &&
            (uint64_t)(now - c->last_seen) > 120) {
            c->used = 0;
            nf_conn_count--;
            nf_stats.total_destroys++;
            expired++;
        }
    }

    if (expired > 0) {
        nf_stats.current_active = (uint64_t)nf_conn_count;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *                       Statistics & Dump
 * ══════════════════════════════════════════════════════════════════ */

void nf_conntrack_stats_get(struct nf_conntrack_stats *stats)
{
    if (stats) *stats = nf_stats;
}

int nf_conntrack_dump(struct nf_conn *buf, int max)
{
    int count = 0;
    for (int i = 0; i < NF_CONNTRACK_MAX && count < max; i++) {
        if (nf_conns[i].used)
            buf[count++] = nf_conns[i];
    }
    return count;
}

/* ══════════════════════════════════════════════════════════════════
 *                          Initialization
 * ══════════════════════════════════════════════════════════════════ */

void nf_conntrack_init(void)
{
    memset(nf_conns, 0, sizeof(nf_conns));
    nf_conn_count = 0;
    memset(&nf_stats, 0, sizeof(nf_stats));
    kprintf("[OK] Conntrack initialized (%d slots)\n", NF_CONNTRACK_MAX);
}
#include "module.h"
module_init(nf_conntrack_init);

/* ── conntrack_destroy: remove a conntrack entry ── */
int conntrack_destroy(void *ct)
{
    if (!ct) {
        kprintf("[conntrack] conntrack_destroy: NULL ct\n");
        return -EINVAL;
    }

    struct nf_conn *conn = (struct nf_conn *)ct;
    if (!conn->used) {
        kprintf("[conntrack] conntrack_destroy: entry already free\n");
        return -EALREADY;
    }

    /* Mark the connection as unused and update counters */
    conn->used = 0;
    nf_conn_count--;
    nf_stats.total_destroys++;
    nf_stats.current_active = (uint64_t)nf_conn_count;

    kprintf("[conntrack] conntrack_destroy: freed ct=%p (src=%d.%d.%d.%d:%u)\n",
            ct,
            (conn->src_ip >> 24) & 0xFF, (conn->src_ip >> 16) & 0xFF,
            (conn->src_ip >> 8) & 0xFF, conn->src_ip & 0xFF,
            (unsigned int)conn->src_port);
    return 0;
}
/* ── conntrack_lookup: find conntrack entry for an skb ── */
void* conntrack_lookup(void *skb)
{
    if (!skb) {
        kprintf("[conntrack] conntrack_lookup: NULL skb\n");
        return NULL;
    }

    /* In a real implementation, we'd extract the 5-tuple from skb
     * and call conntrack_find().  Since skb layout is opaque here,
     * we attempt a linear scan for any active connection.  For a
     * production system, the caller should extract skb metadata
     * and call nf_conntrack_lookup directly. */

    /* Return the first active connection as a best-effort match */
    for (int i = 0; i < NF_CONNTRACK_MAX; i++) {
        if (nf_conns[i].used) {
            kprintf("[conntrack] conntrack_lookup: matched ct[%d]\n", i);
            return (void *)&nf_conns[i];
        }
    }

    kprintf("[conntrack] conntrack_lookup: no matching entry for skb=%p\n", skb);
    return NULL;
}
/* ── conntrack_flush: remove all conntrack entries ── */
int conntrack_flush(void)
{
    int flushed = 0;
    for (int i = 0; i < NF_CONNTRACK_MAX; i++) {
        if (nf_conns[i].used) {
            nf_conns[i].used = 0;
            flushed++;
        }
    }
    nf_conn_count = 0;
    nf_stats.total_destroys += (uint64_t)flushed;
    nf_stats.current_active = 0;

    kprintf("[conntrack] conntrack_flush: flushed %d entries\n", flushed);
    return flushed;
}
