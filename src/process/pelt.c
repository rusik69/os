#define KERNEL_INTERNAL
#include "pelt.h"
#include "timer.h"
#include "printf.h"

/*
 * PELT — Per-Entity Load Tracking Implementation
 *
 * Each task tracks a decaying average of CPU utilisation using an
 * exponential weighted moving average (EWMA) updated every timer tick.
 *
 * The core operation is:
 *   avg = (avg * (HALFLIFE - 1) + new * 1) / HALFLIFE
 *
 * where new is either 0 (idle) or PELT_SCALE (fully utilised).  The
 * result is a fixed-point number in [0, PELT_SCALE] representing the
 * fractional utilisation of a single CPU core.
 *
 * When a task has not been updated for several ticks (e.g. it was
 * sleeping), we apply repeated decay to bring the average down
 * appropriately.
 */

/* ── Internal helpers ──────────────────────────────────────────── */

/*
 * Decay a PELT average by one tick.
 *   avg = (avg * (HALFLIFE - 1)) / HALFLIFE
 */
static inline uint32_t pelt_decay_one(uint32_t avg)
{
    return (uint32_t)(((uint64_t)avg * (PELT_HALFLIFE - 1)) / PELT_HALFLIFE);
}

/*
 * Update a PELT average with a new sample (0 or PELT_SCALE).
 *   avg = (avg * (HALFLIFE - 1) + new * 1) / HALFLIFE
 */
static inline uint32_t pelt_ewma(uint32_t avg, uint32_t new_val)
{
    return (uint32_t)(
        ((uint64_t)avg * (PELT_HALFLIFE - 1) + (uint64_t)new_val) /
        PELT_HALFLIFE
    );
}

/*
 * Apply n rounds of pure decay (no new input).
 * Repeated application of: avg = (avg * (HALFLIFE-1)) / HALFLIFE
 */
static uint32_t pelt_decay_n(uint32_t avg, unsigned int n)
{
    while (n--) {
        avg = pelt_decay_one(avg);
    }
    return avg;
}

/* ── Public API ────────────────────────────────────────────────── */

void pelt_init(struct pelt_state *pelt)
{
    if (!pelt)
        return;

    pelt->util_avg    = PELT_INIT_LOAD;
    pelt->load_avg    = PELT_INIT_LOAD;
    pelt->last_update = (uint32_t)timer_get_ticks();
}

void pelt_update(struct pelt_state *pelt, int running, int runnable,
                 uint64_t now)
{
    uint32_t ticks;
    unsigned int decay_ticks;

    if (!pelt)
        return;

    /* Calculate how many ticks have elapsed since the last update.
     * If zero or negative (wraparound), just decay one tick.
     * Clamp to a reasonable max to avoid extreme decay on long sleeps. */
    ticks = (uint32_t)(now - pelt->last_update);
    if (ticks == 0)
        ticks = 1;
    if (ticks > PELT_HALFLIFE * 4)     /* max 128 ticks decay */
        ticks = PELT_HALFLIFE * 4;

    /* Apply decay for the elapsed ticks minus the current one */
    decay_ticks = ticks - 1;
    if (decay_ticks > 0) {
        pelt->util_avg = pelt_decay_n(pelt->util_avg, decay_ticks);
        pelt->load_avg = pelt_decay_n(pelt->load_avg, decay_ticks);
    }

    /* Update util_avg based on whether the task was running */
    pelt->util_avg = pelt_ewma(pelt->util_avg,
                               running ? PELT_SCALE : 0);

    /* Update load_avg based on whether the task was runnable */
    pelt->load_avg = pelt_ewma(pelt->load_avg,
                               runnable ? PELT_SCALE : 0);

    pelt->last_update = (uint32_t)now;
}

void pelt_decay_missed(struct pelt_state *pelt, unsigned int n_ticks)
{
    if (!pelt || n_ticks == 0)
        return;

    if (n_ticks > PELT_HALFLIFE * 4)
        n_ticks = PELT_HALFLIFE * 4;

    pelt->util_avg = pelt_decay_n(pelt->util_avg, n_ticks);
    pelt->load_avg = pelt_decay_n(pelt->load_avg, n_ticks);
}

/* ── Initialisation call (optional, for status message) ────────── */
void pelt_subsys_init(void)
{
    kprintf("[OK] PELT load tracking initialized (half-life=%d ticks)\n",
            PELT_HALFLIFE);
}
