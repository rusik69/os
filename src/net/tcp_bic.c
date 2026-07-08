/* tcp_bic.c — BIC-TCP congestion control (binary search increase) */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/*
 * BIC-TCP: Binary Increase Congestion Control (RFC 6356 / Linux BIC)
 *
 * BIC uses binary search to find the optimal cwnd value after a loss event,
 * then switches to additive increase when far from the target.
 * This provides good scalability in high-BW environments while remaining
 * TCP-friendly.
 *
 * Key parameters:
 *   - β (beta): multiplicative decrease factor (default: 0.125 → cwnd reduced by 1/8)
 *   - S_{max}: maximum increment (default: 32 segments)
 *   - S_{min}: minimum increment (default: 0.01 segments, scaled to 1)
 *   - W_{max}: target window size saved at each loss event
 */

/* BIC constants */
#define BIC_BETA_SHIFT      3           /* β = 1/8 = 0.125 */
#define BIC_BETA            (1U << BIC_BETA_SHIFT)  /* 8 */
#define BIC_BETA_SCALE      8           /* β is 1/8 */
#define BIC_SMAX            32          /* max increment (segments) */
#define BIC_SMIN            1           /* min increment (segments) */
#define BIC_LOW_WINDOW      14          /* switch to standard TCP when cwnd < 14 */

/* BIC per-connection state */
struct bic_data {
    uint32_t wmax;              /* target window from last congestion */
    uint32_t wmin;              /* lower bound for binary search */
    uint32_t wmid;              /* mid point being probed */
    uint32_t current_cwnd;      /* current congestion window */
    uint32_t ssthresh;          /* slow-start threshold */
    uint64_t epoch_start;       /* tick at current epoch start */
    int      epoch_started;     /* flag: epoch has started */
    int      in_binary_search;  /* 1 if in binary search phase */
    int      initialised;
};

/* Initialize BIC state */
static void bic_init(struct bic_data *b)
{
    if (!b || b->initialised) return;
    memset(b, 0, sizeof(*b));
    b->wmax = 0;
    b->ssthresh = 0x7FFFFFFF;
    b->current_cwnd = 10;       /* initial cwnd = 10 segments */
    b->initialised = 1;
}

/* BIC congestion avoidance update (called per ACK in congestion avoidance) */
static uint32_t bic_update(struct bic_data *b, uint32_t cwnd, int acked_segments)
{
    if (!b || !b->initialised) return cwnd;

    uint32_t target, increment;

    if (b->wmax == 0) {
        /* First loss event not yet recorded — additive increase (TCP-like) */
        target = cwnd + (uint32_t)acked_segments;
    } else {
        /* Binary search increase */
        if (cwnd < b->wmax) {
            /* Below Wmax — binary search zone */
            if (!b->in_binary_search) {
                /* Start a new binary search epoch */
                b->wmin = cwnd;
                b->in_binary_search = 1;
            }

            /* Mid point between Wmin and Wmax */
            b->wmid = b->wmin + (b->wmax - b->wmin) / 2;

            if (b->wmid - cwnd > BIC_SMAX) {
                /* Far from mid — additive increase with S_max */
                increment = BIC_SMAX;
            } else if (b->wmid - cwnd < BIC_SMIN) {
                /* Very close — use minimum increment */
                increment = BIC_SMIN;
            } else {
                /* Standard binary search increment */
                increment = b->wmid - cwnd;
            }

            target = cwnd + increment;
        } else {
            /* Above Wmax — max probing phase */
            increment = BIC_SMAX;
            target = cwnd + increment;
            b->in_binary_search = 0;
        }

        /* Limit by ssthresh */
        if (target > b->ssthresh)
            target = b->ssthresh;
    }

    return target;
}

/* BIC on loss event */
static void bic_on_loss(struct bic_data *b, uint32_t current_cwnd)
{
    if (!b || !b->initialised) return;

    /* Save Wmax for binary search */
    b->wmax = current_cwnd;

    /* Set Wmin = Wmax * (1 - β) = Wmax * 7/8 */
    b->wmin = current_cwnd - (current_cwnd >> BIC_BETA_SHIFT);

    /* Reset binary search flag */
    b->in_binary_search = 0;

    /* Set new ssthresh = Wmax * (1 - β) */
    b->ssthresh = b->wmin;

    /* Reduce cwnd to ssthresh */
    b->current_cwnd = b->ssthresh;

    (void)BIC_BETA_SCALE;
    (void)BIC_LOW_WINDOW;
}

/* Get current BIC cwnd */
static uint32_t bic_get_cwnd(struct bic_data *b)
{
    if (!b || !b->initialised) return 10;
    return b->current_cwnd;
}

/* Set cwnd */
static void bic_set_cwnd(struct bic_data *b, uint32_t cwnd)
{
    if (!b || !b->initialised) return;
    b->current_cwnd = cwnd;
}

/* ── Implement: tcp_bic_cong_avoid ────────────────── */
int tcp_bic_cong_avoid(void *sk) { (void)sk; return 0; }
/* ── Implement: tcp_bic_ssthresh ────────────────── */
uint32_t tcp_bic_ssthresh(void *sk) { (void)sk; return 2; }
