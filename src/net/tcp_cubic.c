/* tcp_cubic.c — CUBIC TCP congestion control (RFC 8312) */

#include "tcp_cubic.h"
#include "types.h"
#include "string.h"
#include "printf.h"

/*
 * CUBIC-TCP: RFC 8312 congestion control using a cubic function
 * for window growth after a congestion event.
 *
 *   W_cubic(t) = C * (t - K)^3 + W_max
 *
 * where t is elapsed time since the last congestion event,
 *       K = cbrt(W_max * (1-beta) / C) is the time to reach W_max again,
 *       beta = 0.7 is the multiplicative-decrease factor,
 *       C = 0.4 is a scaling constant.
 *
 * All arithmetic uses fixed-point with 10 fractional bits (SCALE=10).
 */

/* ── CUBIC constants ─────────────────────────────────────────── */

#define CUBIC_SCALE        10                /* fixed-point scale factor */
#define CUBIC_ONE          (1U << CUBIC_SCALE) /* 1.0 in fixed point */
#define CUBIC_C_FIXED      410               /* C = 0.4 * 1024 */
#define CUBIC_BETA_FIXED   717               /* beta = 0.7 * 1024 */
#define CUBIC_BETA_INV     (CUBIC_ONE - CUBIC_BETA_FIXED) /* (1-beta) = 307 */

/* Standard CUBIC constant for K calculation: (1-beta) / C */
#define CUBIC_K_FACTOR_FIXED ((CUBIC_BETA_INV * CUBIC_ONE) / CUBIC_C_FIXED)

/* Ticks-per-second for time conversion (timer runs at ~100 Hz) */
#define TICKS_PER_SEC      100ULL

/* ── Fixed-point integer cube root (Newton-Raphson, rounds toward zero) ──
 * Returns floor(cbrt(a)) for a > 0.  Uses at most 12 iterations.
 */
static uint32_t cubic_root(uint64_t a)
{
    if (a == 0 || a == 1)
        return (uint32_t)a;

    /* Initial guess: 2^(ceil(log2(a)/3)) */
    uint32_t x;
    if (a < 8)
        x = 2;
    else if (a < 64)
        x = 4;
    else if (a < 512)
        x = 8;
    else if (a < 4096)
        x = 16;
    else if (a < 32768)
        x = 32;
    else if (a < 262144)
        x = 64;
    else if (a < 2097152ULL)
        x = 128;
    else if (a < 16777216ULL)
        x = 256;
    else if (a < 134217728ULL)
        x = 512;
    else
        x = 1024;

    for (int i = 0; i < 12; i++) {
        uint64_t x3 = (uint64_t)x * x * x;
        if (x3 == a)
            return x;
        uint64_t next = (2 * (uint64_t)x + a / (uint64_t)x / x) / 3;
        /* Check for convergence */
        if (next == (uint64_t)x || (next > (uint64_t)x && next - (uint64_t)x <= 1))
            return (uint32_t)next;
        if ((uint64_t)x > next && (uint64_t)x - next <= 1)
            return (uint32_t)next;
        x = (uint32_t)next;
    }
    return x;
}

/* ── Initialize CUBIC state ─────────────────────────────────── */

void cubic_init(struct cubic_data *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->wmax = 0;
    c->use_cubic = 0;
}

/* ── CUBIC window growth computation ────────────────────────────
 *
 * Compute the target congestion window using the cubic function.
 * All time values are in ticks (1 tick ≈ 10 ms).
 *
 * @c      Per-connection CUBIC state
 * @cwnd   Current congestion window (segments)
 * @now    Current tick value
 * @rtt    Smoothed RTT in ticks (for Reno-friendly region)
 * Returns target cwnd in segments.
 */
uint32_t cubic_update(struct cubic_data *c, uint32_t cwnd,
                      uint64_t now, uint32_t rtt_ticks)
{
    if (!c) return cwnd;

    if (c->epoch_start == 0) {
        /* No congestion event yet — behave like Reno */
        return cwnd;
    }

    /* Elapsed time since the start of this epoch (in ticks) */
    uint64_t elapsed_ticks = now - c->epoch_start;
    if (elapsed_ticks == 0)
        return cwnd;

    /* Convert elapsed time to a fixed-point value (units of seconds * 1024).
     * t_sec_fp = elapsed_ticks * 1024 / TICKS_PER_SEC */
    uint64_t t_fp = (elapsed_ticks * CUBIC_ONE) / TICKS_PER_SEC;

    /* Compute K = cbrt(W_max * (1-beta) / C) in the same time units.
     * K is the time at which the cubic function reaches W_max.
     *
     * K_fp = cbrt(W_max * (1-beta)/C * 1024^3) in fixed-point seconds*1024
     *      = cbrt(W_max * CUBIC_K_FACTOR_FIXED * CUBIC_ONE)
     *      = cbrt(W_max * 307 * 1024 / 410 * 1024)
     */
    uint64_t wmax_fp = (uint64_t)c->wmax * CUBIC_K_FACTOR_FIXED;
    uint64_t k_arg = wmax_fp * CUBIC_ONE;
    uint32_t k_fp = cubic_root(k_arg);

    /* Compute t - K (may be negative, handled via signed arithmetic) */
    int64_t delta_fp = (int64_t)t_fp - (int64_t)k_fp;

    /* W_cubic = C * (t - K)^3 + W_max
     *
     * Compute in fixed point:
     *   W_cubic_fp = C * delta_fp^3 / 1024^2 + W_max * 1024
     *              = CUBIC_C_FIXED * delta_fp^3 / 1024^2 / 1024 + W_max * 1024
     *              = CUBIC_C_FIXED * delta_fp^3 / 1073741824 + W_max * 1024
     *
     * Then cwnd = W_cubic_fp / 1024
     */
    int64_t delta3_fp = delta_fp * delta_fp * delta_fp;
    int64_t wcubic_fp;
    if (delta3_fp >= 0) {
        wcubic_fp = ((int64_t)CUBIC_C_FIXED * delta3_fp) / (int64_t)CUBIC_ONE;
        wcubic_fp = wcubic_fp / (int64_t)CUBIC_ONE;
        wcubic_fp += (int64_t)c->wmax * CUBIC_ONE;
    } else {
        /* (t-K) < 0: cubic is decreasing, floor at W_max * (1-beta) */
        uint64_t wmin = (uint64_t)c->wmax * CUBIC_BETA_FIXED / CUBIC_ONE;
        if (wmin < 2) wmin = 2;
        return (uint32_t)wmin;
    }

    /* Convert from fixed-point to integer cwnd (segments) */
    uint32_t target;
    if (wcubic_fp <= 0)
        target = 2;
    else
        target = (uint32_t)((uint64_t)wcubic_fp / CUBIC_ONE);

    /* Ensure minimum cwnd of 2 */
    if (target < 2) target = 2;

    /* Clamp growth: never grow faster than 32 segments per RTT to avoid
     * excessive bursts when far from W_max */
    uint32_t max_growth = (rtt_ticks > 0) ? (32 * elapsed_ticks / rtt_ticks) : 32;
    if (max_growth < 2) max_growth = 2;
    uint32_t max_limit = c->wmax + max_growth;
    if (target > max_limit)
        target = max_limit;

    return target;
}

/* ── Congestion event handler ──────────────────────────────────
 *
 * Called on packet loss (dupack, RTO, timeout).
 * Records W_max for the cubic function, computes beta-reduced ssthresh,
 * and starts a new congestion epoch.
 *
 * Returns the new ssthresh (wmax * beta).
 */
uint32_t cubic_on_loss(struct cubic_data *c, uint32_t current_cwnd,
                       uint64_t now)
{
    if (!c) return 2;

    /* Record W_max at congestion point */
    c->wmax = current_cwnd;

    /* Compute new ssthresh = wmax * beta (multiplicative decrease) */
    uint32_t ssthresh;
    if (current_cwnd > 2)
        ssthresh = (current_cwnd * CUBIC_BETA_FIXED) / CUBIC_ONE;
    else
        ssthresh = 2;

    /* Start a new congestion epoch */
    c->epoch_start = now;
    c->use_cubic = 1;

    return ssthresh;
}

/* ── Accessors ───────────────────────────────────────────────── */

uint32_t cubic_get_wmax(struct cubic_data *c)
{
    if (!c) return 0;
    return c->wmax;
}

void cubic_set_wmax(struct cubic_data *c, uint32_t wmax)
{
    if (!c) return;
    c->wmax = wmax;
}
