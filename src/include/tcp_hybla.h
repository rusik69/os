#ifndef TCP_HYBLA_H
#define TCP_HYBLA_H

#include "types.h"
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

/* Scaling factor for ρ to avoid floating point */
#define HYBLA_SCALE_SHIFT   8
#define HYBLA_SCALE         (1U << HYBLA_SCALE_SHIFT)  /* 256 */

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

/* ── Internal API ──────────────────────────────────────────────────── */

void     hybla_init(struct hybla_data *h);
void     hybla_update_rtt(struct hybla_data *h, uint32_t rtt_ticks);
uint32_t hybla_update(struct hybla_data *h, uint32_t cwnd, int acked_segments);
void     hybla_on_loss(struct hybla_data *h, uint32_t current_cwnd);
uint32_t hybla_get_cwnd(struct hybla_data *h);
void     hybla_set_cwnd(struct hybla_data *h, uint32_t cwnd);

/* ── TCP congestion control callbacks ─────────────────────────────────
 *
 * These are the external entry points that the TCP stack (or a pluggable
 * cc_ops framework) calls during connection processing.  They take a
 * pointer to struct tcp_conn (passed as void *sk for generic interface).
 */

int      tcp_hybla_init(void *sk);
int      tcp_hybla_cong_avoid(void *sk);
uint32_t tcp_hybla_ssthresh(void *sk);
int      tcp_hybla_acked(void *sk, uint32_t acked);

#endif /* TCP_HYBLA_H */
