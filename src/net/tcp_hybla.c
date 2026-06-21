/* tcp_hybla.c — TCP Hybla (satellite links) */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/*
 * TCP Hybla: Congestion control for high-latency/satellite links (RFC 6298 variant).
 *
 * Hybla addresses the RTT unfairness problem of standard TCP by normalising
 * the window increase rate to a reference RTT (25 ms).  This ensures that
 * connections with long RTTs (satellite links: 250-600 ms) get a fair share
 * of bandwidth compared to terrestrial connections.
 *
 * Key mechanisms:
 *   - Normalised increase rate: cwnd += ρ² / cwnd per ACK (instead of 1/cwnd)
 *     where ρ = RTT / RTT₀ (RTT₀ = 25 ms = reference RTT)
 *   - On loss: standard multiplicative decrease (cwnd /= 2)
 *   - Slow-start: cwnd += ρ² per ACK (instead of 1 per ACK)
 *
 * Also implements RFC 3465 (Appropriate Byte Counting) integration.
 */

/* Reference RTT for normalisation (25 ms in ticks at TIMER_FREQ=100) */
#define HYBLA_RTT0          ((TIMER_FREQ * 25) / 1000)  /* 2.5 → 2 ticks */
#define HYBLA_RTT0_MS       25      /* in milliseconds */

/* Hybla per-connection state */
struct hybla_data {
    uint32_t rtt;                   /* current RTT (ticks) */
    uint32_t rtt0;                  /* reference RTT (ticks) */
    uint32_t rho;                   /* normalisation ratio = RTT / RTT0 (scaled) */
    uint32_t rho2;                  /* ρ² (scaled) */

    /* Window */
    uint32_t cwnd;
    uint32_t ssthresh;

    /* RTT tracking */
    uint32_t min_rtt;

    int initialised;
};

/* Scaling factor for ρ to avoid floating point */
#define HYBLA_SCALE_SHIFT   8
#define HYBLA_SCALE         (1 << HYBLA_SCALE_SHIFT)  /* 256 */

void hybla_init(struct hybla_data *h)
{
    if (!h || h->initialised) return;
    memset(h, 0, sizeof(*h));
    h->rtt0 = HYBLA_RTT0;
    h->min_rtt = 0xFFFFFFFF;
    h->cwnd = 4;                    /* smaller initial window for satellite */
    h->ssthresh = 0x7FFFFFFF;
    h->rho = HYBLA_SCALE;           /* ρ = 1 if no RTT measured yet */
    h->rho2 = HYBLA_SCALE;          /* ρ² = 1 */
    h->initialised = 1;
}

/* Update RTT and recalculate ρ */
void hybla_update_rtt(struct hybla_data *h, uint32_t rtt_ticks)
{
    if (!h || !h->initialised || rtt_ticks == 0) return;

    h->rtt = rtt_ticks;

    /* Update minimum RTT */
    if (rtt_ticks < h->min_rtt)
        h->min_rtt = rtt_ticks;

    /* Use min_rtt for ρ calculation (more stable) */
    uint32_t rtt_for_rho = (h->min_rtt != 0xFFFFFFFF) ? h->min_rtt : rtt_ticks;

    /* Calculate ρ = RTT / RTT0 (fixed-point: ρ * HYBLA_SCALE) */
    if (h->rtt0 > 0) {
        h->rho = (rtt_for_rho * HYBLA_SCALE) / h->rtt0;

        /* ρ² = ρ * ρ / HYBLA_SCALE (re-normalise) */
        h->rho2 = (h->rho * h->rho) / HYBLA_SCALE;

        if (h->rho2 < HYBLA_SCALE)
            h->rho2 = HYBLA_SCALE;  /* minimum ρ² = 1 */
    }
}

/* Hybla congestion avoidance update (per ACK) */
uint32_t hybla_update(struct hybla_data *h, uint32_t cwnd, int acked_segments)
{
    if (!h || !h->initialised) return cwnd;

    if (cwnd < h->ssthresh) {
        /* Slow start: cwnd += ρ² per ACK */
        uint32_t inc = (h->rho2 * (uint32_t)acked_segments) / HYBLA_SCALE;
        if (inc < 1) inc = 1;
        cwnd += inc;
    } else {
        /* Congestion avoidance: cwnd += ρ² / cwnd per ACK */
        /* Since we process multiple ACKs: cwnd += ρ² * acked / cwnd */
        uint32_t inc = (h->rho2 * (uint32_t)acked_segments) / cwnd;
        if (inc < 1) inc = 1;
        /* Clamp to avoid excessive growth */
        if (inc > (uint32_t)acked_segments)
            inc = (uint32_t)acked_segments;
        cwnd += inc;
    }

    if (cwnd > 0xFFFF) cwnd = 0xFFFF;  /* cap at an extreme max */

    h->cwnd = cwnd;
    return cwnd;
}

/* Hybla on loss: standard multiplicative decrease */
void hybla_on_loss(struct hybla_data *h, uint32_t current_cwnd)
{
    if (!h || !h->initialised) return;

    h->ssthresh = current_cwnd / 2;
    if (h->ssthresh < 2) h->ssthresh = 2;
    h->cwnd = h->ssthresh;
}

uint32_t hybla_get_cwnd(struct hybla_data *h)
{
    if (!h || !h->initialised) return 4;
    return h->cwnd;
}

void hybla_set_cwnd(struct hybla_data *h, uint32_t cwnd)
{
    if (!h || !h->initialised) return;
    h->cwnd = cwnd;
}

/* ── Stub: tcp_hybla_cong_avoid ─────────────────────────────── */
int tcp_hybla_cong_avoid(void *sk)
{
    (void)sk;
    kprintf("[tcp_hybla] tcp_hybla_cong_avoid: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: tcp_hybla_ssthresh ─────────────────────────────── */
uint32_t tcp_hybla_ssthresh(void *sk)
{
    (void)sk;
    kprintf("[tcp_hybla] tcp_hybla_ssthresh: not yet implemented\n");
    return -ENOSYS;
}
