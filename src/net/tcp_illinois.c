/* tcp_illinois.c — TCP Illinois (delay-based + loss-based) */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/*
 * TCP Illinois: Hybrid delay-based and loss-based congestion control (Liu et al., 2007).
 *
 * Illinois combines delay-based (α, β) and loss-based AIMD parameters.
 * It uses RTT measurements to adapt the aggressiveness of window increase
 * and the responsiveness of window decrease.
 *
 * When queueing delay is low (no congestion):
 *   - α increases (more aggressive increase)
 *   - β decreases (less drastic reduction)
 * When queueing delay is high (congestion approaching):
 *   - α decreases (smaller additive increase)
 *   - β increases (more drastic reduction on loss)
 *
 * This allows high throughput on paths with large buffers while maintaining
 * TCP-friendliness and RTT fairness.
 */

/* Illinois constants */
#define ILLINOIS_ALPHA_MIN      1       /* minimum α (increase factor) */
#define ILLINOIS_ALPHA_MAX      10      /* maximum α */
#define ILLINOIS_ALPHA_BASE     8       /* default α */
#define ILLINOIS_BETA_MIN       512     /* minimum β (decrease factor, scaled) */
#define ILLINOIS_BETA_MAX       1536    /* maximum β */
#define ILLINOIS_BETA_BASE      1024    /* default β (represents 0.5 after scaling) */
#define ILLINOIS_THETA          5       /* queueing delay threshold (ticks) */
#define ILLINOIS_THETA_LOW      2       /* low queueing delay threshold */
#define ILLINOIS_THETA_HIGH     15      /* high queueing delay threshold */
#define ILLINOIS_SCALE          1024    /* scaling factor for α, β */

/* Illinois per-connection state */
struct illinois_data {
    /* RTT tracking */
    uint32_t base_rtt;              /* minimum RTT (ticks) */
    uint32_t current_rtt;           /* latest RTT sample */

    /* Current AIMD parameters */
    uint32_t alpha;                 /* additive increase (scaled) */
    uint32_t beta;                  /* multiplicative decrease (scaled) */

    /* Queueing delay */
    uint32_t queue_delay;           /* current RTT - base RTT */

    /* Window */
    uint32_t cwnd;
    uint32_t ssthresh;

    int initialised;
};

void illinois_init(struct illinois_data *d)
{
    if (!d || d->initialised) return;
    memset(d, 0, sizeof(*d));
    d->base_rtt = 0xFFFFFFFF;
    d->alpha = ILLINOIS_ALPHA_BASE;
    d->beta = ILLINOIS_BETA_BASE;
    d->cwnd = 10;
    d->ssthresh = 0x7FFFFFFF;
    d->initialised = 1;
}

/* Update RTT and adapt α, β based on queueing delay */
void illinois_update_rtt(struct illinois_data *d, uint32_t rtt_ticks)
{
    if (!d || !d->initialised || rtt_ticks == 0) return;

    d->current_rtt = rtt_ticks;

    /* Update base RTT (minimum observed) */
    if (rtt_ticks < d->base_rtt)
        d->base_rtt = rtt_ticks;

    /* Calculate queueing delay */
    if (d->base_rtt != 0xFFFFFFFF && d->base_rtt > 0) {
        if (rtt_ticks > d->base_rtt) {
            d->queue_delay = rtt_ticks - d->base_rtt;
        } else {
            d->queue_delay = 0;
        }

        /* Adapt α based on queueing delay:
         *   - Low delay (θ_low) → α = α_max (aggressive)
         *   - High delay (θ_high) → α = α_min (conservative)
         *   - In between → linear interpolation
         */
        if (d->queue_delay <= ILLINOIS_THETA_LOW) {
            d->alpha = ILLINOIS_ALPHA_MAX;
        } else if (d->queue_delay >= ILLINOIS_THETA_HIGH) {
            d->alpha = ILLINOIS_ALPHA_MIN;
        } else {
            /* Linear: α_max - (delay - θ_low) / (θ_high - θ_low) * (α_max - α_min) */
            uint32_t range = ILLINOIS_THETA_HIGH - ILLINOIS_THETA_LOW;
            uint32_t offset = d->queue_delay - ILLINOIS_THETA_LOW;
            d->alpha = ILLINOIS_ALPHA_MAX -
                       (offset * (ILLINOIS_ALPHA_MAX - ILLINOIS_ALPHA_MIN)) / range;
        }

        /* Adapt β based on queueing delay:
         *   - Low delay → β = β_min (mild reduction)
         *   - High delay → β = β_max (strong reduction)
         */
        if (d->queue_delay <= ILLINOIS_THETA_LOW) {
            d->beta = ILLINOIS_BETA_MIN;
        } else if (d->queue_delay >= ILLINOIS_THETA_HIGH) {
            d->beta = ILLINOIS_BETA_MAX;
        } else {
            uint32_t range = ILLINOIS_THETA_HIGH - ILLINOIS_THETA_LOW;
            uint32_t offset = d->queue_delay - ILLINOIS_THETA_LOW;
            d->beta = ILLINOIS_BETA_MIN +
                      (offset * (ILLINOIS_BETA_MAX - ILLINOIS_BETA_MIN)) / range;
        }
    }
}

/* Illinois congestion avoidance update (per ACK) */
uint32_t illinois_update(struct illinois_data *d, uint32_t cwnd,
                         int acked_segments)
{
    if (!d || !d->initialised) return cwnd;

    if (cwnd < d->ssthresh) {
        /* Slow start */
        cwnd += (uint32_t)acked_segments;
    } else {
        /* Congestion avoidance with α-adaptive increase */
        /* cwnd += α / cwnd (per ACK) */
        uint32_t increase = (d->alpha * ILLINOIS_SCALE) /
                            (cwnd * ILLINOIS_SCALE / (uint32_t)acked_segments + 1);
        if (increase < 1) increase = 1;
        cwnd += increase;
    }

    d->cwnd = cwnd;
    return cwnd;
}

/* Illinois on loss */
void illinois_on_loss(struct illinois_data *d, uint32_t current_cwnd)
{
    if (!d || !d->initialised) return;

    /* ssthresh = cwnd * (1 - β/1024) */
    uint32_t reduction = (current_cwnd * d->beta) / ILLINOIS_SCALE;
    uint32_t new_cwnd;
    if (reduction >= current_cwnd)
        new_cwnd = current_cwnd / 2;  /* fallback */
    else
        new_cwnd = current_cwnd - reduction;

    if (new_cwnd < 2) new_cwnd = 2;

    d->ssthresh = new_cwnd;
    d->cwnd = d->ssthresh;
}

uint32_t illinois_get_cwnd(struct illinois_data *d)
{
    if (!d || !d->initialised) return 10;
    return d->cwnd;
}

void illinois_set_cwnd(struct illinois_data *d, uint32_t cwnd)
{
    if (!d || !d->initialised) return;
    d->cwnd = cwnd;
}
