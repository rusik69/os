#include "ratelimit_ext.h"
#include "printf.h"
#include "kernel.h"
#include "timer.h"

/*
 * Extended ratelimit implementation.
 *
 * Uses timer_get_ticks() for time-stamping.  The caller is responsible
 * for ensuring the timer subsystem is initialised before use.
 */

int ___ratelimit(struct ratelimit_state *state, const char *func)
{
    uint64_t now;

    if (!state)
        return 1;  /* no limit */

    now = timer_get_ticks();

    if (state->interval <= 0)
        state->interval = 5;
    if (state->burst <= 0)
        state->burst = 10;

    /* Time window rolled over? */
    if (now - state->begin >= (uint64_t)state->interval) {
        state->begin   = now;
        state->printed = 0;
    }

    /* Miss tracking: count suppressed messages */
    if (state->printed >= state->burst) {
        state->missed++;
        return 0;  /* suppress */
    }

    state->printed++;
    return 1;  /* allow */
}

void ratelimit_ext_init(void)
{
    kprintf("[OK] ratelimit_ext: Extended ratelimit initialised\n");
}
