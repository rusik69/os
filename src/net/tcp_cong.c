/* tcp_cong.c — TCP congestion control framework abstraction
 *
 * Provides a pluggable congestion control operations structure
 * allowing different algorithms (CUBIC, BBR, Reno, etc.) to be
 * selected per-connection or globally.
 *
 * Architecture:
 *   struct tcp_congestion_ops — virtual table for CC operations
 *   tcp_cong_register()      — register a CC algorithm
 *   tcp_cong_select()        — select a CC for a connection
 *   Built-in: CUBIC (default), Reno (fallback), BBR (if available)
 */

#include "types.h"
#include "string.h"
#include "printf.h"
#include "net_tcp.h"
#include "tcp_cong.h"
#include "errno.h"

/* ── Configuration ─────────────────────────────────────────────────── */

#define TCP_CC_MAX_ALGOS   16

/* ── Global congestion control registry ────────────────────────────── */

static struct tcp_congestion_ops *g_cc_algos[TCP_CC_MAX_ALGOS];
static int g_cc_num_algos = 0;

/* Default congestion control algorithm name */
static const char *g_cc_default = "cubic";

/* ── Currently selected CC per connection ──────────────────────────── */

/* Simple: for now all connections use the same CC */
static struct tcp_congestion_ops *g_current_cc = NULL;

/* ── Built-in CUBIC congestion control ─────────────────────────────── */

static uint32_t cubic_cwnd(struct tcp_sock *tp)
{
    (void)tp;
    return tp->snd_cwnd;
}

static void cubic_cwnd_update(struct tcp_sock *tp, int event)
{
    (void)event;
    /* Simplified CUBIC: increase cwnd by 1 per RTT in congestion avoidance */
    if (tp->tcp_state == TCP_ESTABLISHED && !event) {
        tp->snd_cwnd += 1;
        if (tp->snd_cwnd > tp->snd_cwnd_max)
            tp->snd_cwnd = tp->snd_cwnd_max;
    }
}

static void cubic_cwnd_event(struct tcp_sock *tp, int event_type)
{
    switch (event_type) {
    case TCP_CA_EVENT_LOSS:
        /* On loss: cwnd = cwnd * 0.7 (beta) */
        tp->snd_cwnd = (tp->snd_cwnd * 7) / 10;
        if (tp->snd_cwnd < 2) tp->snd_cwnd = 2;
        break;
    case TCP_CA_EVENT_ECN_NO:
        break;
    case TCP_CA_EVENT_RTT:
        break;
    default:
        break;
    }
}

static int cubic_init(struct tcp_sock *tp)
{
    tp->snd_cwnd = 10;     /* initial window (IW10) */
    tp->snd_cwnd_max = 65535;
    (void)tp;
    return 0;
}

static struct tcp_congestion_ops tcp_cubic_cong = {
    .name       = "cubic",
    .owner      = NULL,
    .flags      = TCP_CC_FLAG_CUBIC,
    .init       = cubic_init,
    .cwnd       = cubic_cwnd,
    .cwnd_update = cubic_cwnd_update,
    .cwnd_event = cubic_cwnd_event,
    .ssthresh   = NULL,
    .cong_avoid = NULL,
    .min_cwnd   = NULL,
    .next       = NULL,
};

/* ── Built-in Reno congestion control ──────────────────────────────── */

static uint32_t reno_cwnd(struct tcp_sock *tp)
{
    return tp->snd_cwnd;
}

static void reno_cwnd_update(struct tcp_sock *tp, int event)
{
    (void)event;
    if (tp->tcp_state == TCP_ESTABLISHED) {
        tp->snd_cwnd += 1;
    }
}

static void reno_cwnd_event(struct tcp_sock *tp, int event_type)
{
    if (event_type == TCP_CA_EVENT_LOSS) {
        tp->snd_cwnd = tp->snd_cwnd / 2;
        if (tp->snd_cwnd < 2) tp->snd_cwnd = 2;
    }
}

static int reno_init(struct tcp_sock *tp)
{
    tp->snd_cwnd = 10;
    tp->snd_cwnd_max = 65535;
    (void)tp;
    return 0;
}

static struct tcp_congestion_ops tcp_reno_cong = {
    .name        = "reno",
    .owner       = NULL,
    .flags       = TCP_CC_FLAG_RENO,
    .init        = reno_init,
    .cwnd        = reno_cwnd,
    .cwnd_update = reno_cwnd_update,
    .cwnd_event  = reno_cwnd_event,
    .ssthresh    = NULL,
    .cong_avoid  = NULL,
    .min_cwnd    = NULL,
    .next        = NULL,
};

/* ── Registration API ──────────────────────────────────────────────── */

int tcp_cong_register(struct tcp_congestion_ops *ops)
{
    if (!ops || !ops->name) return -EINVAL;
    if (g_cc_num_algos >= TCP_CC_MAX_ALGOS) return -ENOSPC;

    /* Check for duplicate */
    for (int i = 0; i < g_cc_num_algos; i++) {
        if (strcmp(g_cc_algos[i]->name, ops->name) == 0)
            return -EEXIST;
    }

    g_cc_algos[g_cc_num_algos++] = ops;
    kprintf("[TCP_CC] Registered CC '%s'\n", ops->name);

    /* If this is the first registration, make it default */
    if (g_current_cc == NULL) {
        g_current_cc = ops;
    }

    return g_cc_num_algos - 1;
}

/* ── Selection API ─────────────────────────────────────────────────── */

struct tcp_congestion_ops *tcp_cong_find(const char *name)
{
    if (!name) return NULL;

    for (int i = 0; i < g_cc_num_algos; i++) {
        if (strcmp(g_cc_algos[i]->name, name) == 0)
            return g_cc_algos[i];
    }

    return NULL;
}

int tcp_cong_select(struct tcp_sock *tp, const char *name)
{
    if (!tp) return -EINVAL;

    struct tcp_congestion_ops *ops = tcp_cong_find(name);
    if (!ops) return -ENOENT;

    tp->cong_ops = ops;

    /* Initialize the CC for this connection */
    if (ops->init) {
        return ops->init(tp);
    }

    return 0;
}

/* ── Get current CC name ───────────────────────────────────────────── */

const char *tcp_cong_get_name(const struct tcp_sock *tp)
{
    if (tp && tp->cong_ops)
        return tp->cong_ops->name;
    return g_cc_default;
}

/* ── List registered CC algorithms ─────────────────────────────────── */

int tcp_cong_list(char *buf, int buf_len)
{
    int pos = 0;
    for (int i = 0; i < g_cc_num_algos && pos < buf_len; i++) {
        int n = snprintf(buf + pos, (size_t)(buf_len - pos),
                         "%s%s\n",
                         g_cc_algos[i]->name,
                         (g_cc_algos[i] == g_current_cc) ? " (default)" : "");
        if (n > 0) pos += n;
    }
    return pos;
}

/* ── Apply CC to a connection ──────────────────────────────────────── */

int tcp_cong_apply(struct tcp_sock *tp)
{
    if (!tp) return -EINVAL;

    struct tcp_congestion_ops *ops = tp->cong_ops;
    if (!ops) {
        /* Default to CUBIC if none selected */
        ops = tcp_cong_find("cubic");
        if (!ops) return -ENOENT;
        tp->cong_ops = ops;
    }

    if (ops->init)
        return ops->init(tp);

    return 0;
}

/* ── Initialization ────────────────────────────────────────────────── */

void tcp_cong_init(void)
{
    /* Register built-in algorithms */
    tcp_cong_register(&tcp_cubic_cong);
    tcp_cong_register(&tcp_reno_cong);

    /* Try to register BBR if available (from tcp_bbr.c) */
    /* tcp_bbr_register(); — called separately if BBR is compiled in */

    kprintf("[OK] TCP_CONG: pluggable congestion control framework (%d algos)\n",
            g_cc_num_algos);
}
