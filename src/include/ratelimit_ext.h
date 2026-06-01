#ifndef RATELIMIT_EXT_H
#define RATELIMIT_EXT_H

#include "types.h"

/*
 * Extended ratelimit state and helper.
 *
 * This is an extension / alternative to the basic ratelimit in ratelimit.h.
 * It provides a richer ratelimit_state with separate burst, interval,
 * and a miss counter, plus a convenience init macro.
 */

struct ratelimit_state {
    uint64_t  begin;        /* timestamp (ticks) when window started */
    int       interval;     /* window length (in timer ticks) */
    int       burst;        /* max messages in window */
    int       printed;      /* messages printed in current window */
    int       missed;       /* messages suppressed */
    int       flags;        /* reserved */
};

/*
 * ratelimit_state_init  - Statically initialise a ratelimit_state.
 * Usage: struct ratelimit_state rs = RATELIMIT_STATE_INIT(interval, burst);
 */
#define RATELIMIT_STATE_INIT(interval_, burst_) \
    { .begin = 0, .interval = (interval_), .burst = (burst_), \
      .printed = 0, .missed = 0, .flags = 0 }

/*
 * ___ratelimit  - Core ratelimit check.
 * Returns 1 if the message should be printed, 0 if suppressed.
 * Updates missed counter when suppressing.
 *
 * 'state' must be pre-initialised (via RATELIMIT_STATE_INIT or
 * ratelimit_state_init function).
 */
int ___ratelimit(struct ratelimit_state *state, const char *func);

/*
 * ratelimit_state_init_func  - Runtime initialisation of a state.
 */
static inline void ratelimit_state_init(struct ratelimit_state *state,
                                        int interval, int burst)
{
    state->begin    = 0;
    state->interval = interval;
    state->burst    = burst;
    state->printed  = 0;
    state->missed   = 0;
    state->flags    = 0;
}

void ratelimit_ext_init(void);

#endif /* RATELIMIT_EXT_H */
