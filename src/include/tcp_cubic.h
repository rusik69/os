#ifndef TCP_CUBIC_H
#define TCP_CUBIC_H

#include "types.h"

/* ── CUBIC per-connection state (embedded directly in struct tcp_conn) ── */

struct cubic_data {
    uint32_t wmax;              /* W_max: cwnd at last congestion event */
    uint64_t epoch_start;       /* tick when the current epoch started */
    uint32_t origin_point;      /* origin point for cubic growth */
    int      use_cubic;         /* 1 = using CUBIC algorithm (epoch established) */
};

/* ── CUBIC API ─────────────────────────────────────────────────────────── */

/* Allocate and initialize CUBIC state */
void     cubic_init(struct cubic_data *c);

/*
 * Compute target congestion window using the cubic function.
 * Called per ACK during congestion avoidance.
 * Returns target cwnd in segments.
 */
uint32_t cubic_update(struct cubic_data *c, uint32_t cwnd,
                      uint64_t now, uint32_t rtt_ticks);

/*
 * Handle a congestion event (loss, dupack, RTO).
 * Sets wmax, epoch_start, use_cubic, and returns the new ssthresh.
 */
uint32_t cubic_on_loss(struct cubic_data *c, uint32_t current_cwnd,
                       uint64_t now);

/* Return current W_max */
uint32_t cubic_get_wmax(struct cubic_data *c);

/* Set W_max (for idle restart tracking) */
void     cubic_set_wmax(struct cubic_data *c, uint32_t wmax);

#endif /* TCP_CUBIC_H */
