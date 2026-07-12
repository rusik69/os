/*
 * psi.c — Pressure Stall Information (PSI)
 *
 * Implements production-quality tracking of resource stall times
 * (CPU, memory, I/O) with exponential moving averages for three
 * window sizes (10s, 60s, 300s).  Exposed via /proc/pressure/.
 *
 * Averaging algorithm (Linux-compatible):
 *   avg' = avg + (delta / window_s * (sample - avg))
 *
 * where delta is the elapsed time since the last update, window_s
 * is the averaging window in seconds, and sample is the stall ratio
 * (some_ticks / wall_ticks) observed in this update period.
 *
 * We use fixed-point arithmetic (FRAC_BITS = 12, ~0.024% resolution)
 * to avoid floating-point entirely in the kernel.
 *
 * Reference: kernel/sched/psi.c (Linux kernel)
 */

#define KERNEL_INTERNAL
#include "psi.h"
#include "printf.h"
#include "string.h"
#include "timer.h"      /* timer_get_ticks(), TIMER_FREQ */
#include "timers.h"     /* timer_schedule() for periodic update */
#include "spinlock.h"
#include "export.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  Fixed-point arithmetic
 * ═══════════════════════════════════════════════════════════════════════ */

#define PSI_FRAC_BITS  12          /* fixed-point resolution: 1/4096 */
#define PSI_FRAC_ONE   (1U << PSI_FRAC_BITS)  /* 1.0 in fixed-point  */
#define PSI_FRAC_HALF  (PSI_FRAC_ONE / 2)    /* 0.5 for rounding    */

/* Format a fixed-point percentage value into a string like "12.34".
 * Always writes exactly 5 characters plus null: "XX.XX" or "X.XX" or "0.00". */
static void psi_format_pct(char *buf, int val_fp)
{
    unsigned int v = (unsigned int)val_fp;
    unsigned int int_part = v >> PSI_FRAC_BITS;
    /* Compute fraction * 100, rounded */
    unsigned int frac = ((v & (PSI_FRAC_ONE - 1)) * 100 + PSI_FRAC_HALF) >> PSI_FRAC_BITS;
    if (frac >= 100) { int_part++; frac = 0; }
    buf[0] = '0' + (char)(int_part / 10);
    buf[1] = '0' + (char)(int_part % 10);
    buf[2] = '.';
    buf[3] = '0' + (char)(frac / 10);
    buf[4] = '0' + (char)(frac % 10);
    buf[5] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Per-resource PSI state
 * ═══════════════════════════════════════════════════════════════════════ */

struct psi_window {
    uint64_t  window_ns;       /* window duration in ns (e.g. 10*1e9) */
    int       window_fp;       /* window duration in fixed-point: window_s * FRAC_ONE */
    int       avg_fp;          /* current moving average (fixed-point percentage) */
};

struct psi_resource {
    spinlock_t lock;

    /* Cumulative stall time (in timer ticks, = 10 ms each) */
    uint64_t  some_total;      /* total ticks with at least one task stalled */
    uint64_t  full_total;      /* total ticks with all tasks stalled */

    /* Per-window averages (some and full) */
    struct psi_window windows[PSI_NUM_WINDOWS];
    struct psi_window full_windows[PSI_NUM_WINDOWS];

    /* Last-update tick for delta calculation */
    uint64_t  last_ticks;
    int       initialized;

    /* ── Concurrency tracking ──────────────────────────────────────────
     * Number of tasks currently stalled on this resource.
     * Incremented by psi_*_enter(), decremented by psi_*_leave().
     * psi_update() uses these to derive some / full ratios.
     *
     * "some"  = (count > 0)            — at least one task stalled
     * "full"  = (count == nr_tasks)    — every non-idle task stalled
     * "none"  = (count == 0)           — no task stalled
     */
    int       stall_count;      /* current number of stalled tasks */
};

/* ── Static state ──────────────────────────────────────────────────── */

static struct psi_resource psi_resources[PSI_NUM_RESOURCES];

/* Window sizes in seconds, matching PSI_WINDOW_* constants */
static const int psi_window_sizes[PSI_NUM_WINDOWS] = {
    PSI_WINDOW_10S,
    PSI_WINDOW_60S,
    PSI_WINDOW_300S
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Moving-average update
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Update one averaging window with a new stall ratio sample.
 *
 * @win:       the per-window state
 * @delta_us:  elapsed microseconds since last update
 * @ratio_fp:  observed stall ratio (some/full) in fixed-point (0..FRAC_ONE)
 *
 * Exponential moving average:
 *   avg' = avg + (dt / window) * (sample - avg)
 *
 * In fixed-point:  avg += (dt * (sample - avg)) / window
 * where dt and window are in the same time units (microseconds).
 *
 * To avoid overflow, we compute in 64-bit with intermediate division.
 */
static void psi_update_window(struct psi_window *win,
                              uint64_t delta_us, int ratio_fp)
{
    if (win->window_ns == 0)
        return;

    /* We need delta / window as a fixed-point fraction.
     * Compute dt_fp = (delta_us * FRAC_ONE) / window_ns  */
    uint64_t window_us = win->window_ns / 1000;  /* convert ns to us */
    if (window_us == 0)
        return;

    /* dt_fp = (delta_us << FRAC_BITS) / window_us */
    uint64_t dt_fp = (delta_us << PSI_FRAC_BITS) / window_us;
    if (dt_fp > PSI_FRAC_ONE)
        dt_fp = PSI_FRAC_ONE;  /* cap at 1.0 */

    /* avg' = avg + dt_fp * (ratio_fp - avg) / FRAC_ONE */
    int delta_avg = (int)(((int64_t)dt_fp * (int64_t)(ratio_fp - win->avg_fp))
                          >> PSI_FRAC_BITS);
    win->avg_fp += delta_avg;

    /* Clamp to [0, FRAC_ONE] */
    if (win->avg_fp < 0)
        win->avg_fp = 0;
    if (win->avg_fp > (int)PSI_FRAC_ONE)
        win->avg_fp = (int)PSI_FRAC_ONE;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

void psi_init(void)
{
    memset(psi_resources, 0, sizeof(psi_resources));

    for (int r = 0; r < PSI_NUM_RESOURCES; r++) {
        struct psi_resource *pr = &psi_resources[r];
        spinlock_init(&pr->lock);
        pr->last_ticks = timer_get_ticks();
        pr->initialized = 1;

        for (int w = 0; w < PSI_NUM_WINDOWS; w++) {
            pr->windows[w].window_ns = (uint64_t)psi_window_sizes[w] * 1000000000ULL;
            pr->windows[w].window_fp = psi_window_sizes[w] * PSI_FRAC_ONE;
            pr->windows[w].avg_fp = 0;
            pr->full_windows[w].window_ns = (uint64_t)psi_window_sizes[w] * 1000000000ULL;
            pr->full_windows[w].window_fp = psi_window_sizes[w] * PSI_FRAC_ONE;
            pr->full_windows[w].avg_fp = 0;
        }
    }

    kprintf("[OK] PSI: pressure stall information initialized "
            "(%d resources, %d averaging windows)\n",
            PSI_NUM_RESOURCES, PSI_NUM_WINDOWS);
}

void psi_update(int resource, uint64_t wall_ticks,
                uint64_t some_ticks, uint64_t full_ticks)
{
    if (resource < 0 || resource >= PSI_NUM_RESOURCES)
        return;

    struct psi_resource *pr = &psi_resources[resource];
    if (!pr->initialized)
        return;

    if (wall_ticks == 0)
        return;

    uint64_t flags;
    spinlock_irqsave_acquire(&pr->lock, &flags);

    /* Update cumulative totals (in timer ticks) */
    pr->some_total += some_ticks;
    pr->full_total += full_ticks;

    /* Compute stall ratios for this period in fixed-point */
    int some_ratio_fp;
    if (some_ticks >= wall_ticks) {
        some_ratio_fp = PSI_FRAC_ONE;
    } else {
        some_ratio_fp = (int)((some_ticks << PSI_FRAC_BITS) / wall_ticks);
    }
    int full_ratio_fp;
    if (full_ticks >= wall_ticks) {
        full_ratio_fp = PSI_FRAC_ONE;
    } else {
        full_ratio_fp = (int)((full_ticks << PSI_FRAC_BITS) / wall_ticks);
    }

    /* Compute elapsed time since last update (in microseconds) */
    uint64_t now_ticks = timer_get_ticks();
    uint64_t elapsed_us = 0;
    if (now_ticks > pr->last_ticks) {
        uint64_t elapsed_ticks = now_ticks - pr->last_ticks;
        /* Convert from timer ticks to microseconds (100 Hz → 10 ms / tick) */
        elapsed_us = elapsed_ticks * (1000000ULL / TIMER_FREQ);
    }
    pr->last_ticks = now_ticks;

    /* Update all averaging windows */
    for (int w = 0; w < PSI_NUM_WINDOWS; w++) {
        /* Some average */
        psi_update_window(&pr->windows[w], elapsed_us, some_ratio_fp);
        /* Full average */
        psi_update_window(&pr->full_windows[w], elapsed_us, full_ratio_fp);
    }

    spinlock_irqsave_release(&pr->lock, flags);
}

int psi_gen_proc_file(int resource, char *buf, int max)
{
    if (resource < 0 || resource >= PSI_NUM_RESOURCES)
        return -1;
    if (!buf || max < 80)
        return -1;

    struct psi_resource *pr = &psi_resources[resource];
    if (!pr->initialized)
        return -1;

    uint64_t flags;
    spinlock_irqsave_acquire(&pr->lock, &flags);

    /* Capture the current averages while holding the lock */
    int avg10_fp = pr->windows[0].avg_fp;
    int avg60_fp = pr->windows[1].avg_fp;
    int avg300_fp = pr->windows[2].avg_fp;
    int full_avg10_fp = pr->full_windows[0].avg_fp;
    int full_avg60_fp = pr->full_windows[1].avg_fp;
    int full_avg300_fp = pr->full_windows[2].avg_fp;

    /* Total stall time (in timer ticks) → convert to microseconds */
    uint64_t some_total_us = pr->some_total * (1000000ULL / TIMER_FREQ);
    uint64_t full_total_us = pr->full_total * (1000000ULL / TIMER_FREQ);

    spinlock_irqsave_release(&pr->lock, flags);

    /* Format the output buffer.
     * Linux format:
     *   some avg10=0.00 avg60=0.00 avg300=0.00 total=0
     *   full avg10=0.00 avg60=0.00 avg300=0.00 total=0
     */
    char some_10[8], some_60[8], some_300[8];
    char full_10[8], full_60[8], full_300[8];

    psi_format_pct(some_10, avg10_fp);
    psi_format_pct(some_60, avg60_fp);
    psi_format_pct(some_300, avg300_fp);

    psi_format_pct(full_10, full_avg10_fp);
    psi_format_pct(full_60, full_avg60_fp);
    psi_format_pct(full_300, full_avg300_fp);

    int pos = 0;
    {
        int n = snprintf(buf + pos, (size_t)(max - pos),
                         "some avg10=%s avg60=%s avg300=%s total=%llu\n",
                         some_10, some_60, some_300,
                         (unsigned long long)some_total_us);
        if (n > 0 && pos + n < max - 1) pos += n;
    }
    if (pos >= max - 1) return max - 1;

    {
        int n = snprintf(buf + pos, (size_t)(max - pos),
                     "full avg10=%s avg60=%s avg300=%s total=%llu\n",
                     full_10, full_60, full_300,
                     (unsigned long long)full_total_us);
        if (n > 0 && pos + n < max) pos += n;
    }
    if (pos >= max) pos = max - 1;
    buf[pos] = '\0';
    return pos;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Enter / leave stall-state markers
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Each resource has a `stall_count` that is incremented on stall entry
 * and decremented on stall exit.  The scheduler tick (or a periodic
 * updater) reads these counts to determine "some" vs "full" pressure.
 *
 *  some  = (stall_count > 0)
 *  full  = (stall_count >= num_active_tasks)
 *  none  = (stall_count == 0)
 */

/*
 * Helper: increment the stall counter for a resource.
 * Returns the new count (caller can use for inlined some/full check).
 */
static inline int psi_inc(int res)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&psi_resources[res].lock, &flags);
    if (psi_resources[res].stall_count < INT32_MAX)
        psi_resources[res].stall_count++;
    int new = psi_resources[res].stall_count;
    spinlock_irqsave_release(&psi_resources[res].lock, flags);
    return new;
}

/*
 * Helper: decrement the stall counter for a resource.
 * Returns the new count.
 */
static inline int psi_dec(int res)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&psi_resources[res].lock, &flags);
    int new = --psi_resources[res].stall_count;
    if (new < 0)
        new = psi_resources[res].stall_count = 0;   /* safety clamp */
    spinlock_irqsave_release(&psi_resources[res].lock, flags);
    return new;
}

/* ── CPU ──────────────────────────────────────────────────────────── */

void psi_cpu_enter(void)
{
    psi_inc(PSI_RES_CPU);
}

void psi_cpu_leave(void)
{
    psi_dec(PSI_RES_CPU);
}

EXPORT_SYMBOL(psi_cpu_enter);
EXPORT_SYMBOL(psi_cpu_leave);

/* ── Memory ───────────────────────────────────────────────────────── */

void psi_memstall_enter(void)
{
    psi_inc(PSI_RES_MEMORY);
}

void psi_memstall_leave(void)
{
    psi_dec(PSI_RES_MEMORY);
}

EXPORT_SYMBOL(psi_memstall_enter);
EXPORT_SYMBOL(psi_memstall_leave);

/* ── IO ───────────────────────────────────────────────────────────── */

void psi_io_enter(void)
{
    psi_inc(PSI_RES_IO);
}

void psi_io_leave(void)
{
    psi_dec(PSI_RES_IO);
}

EXPORT_SYMBOL(psi_io_enter);
EXPORT_SYMBOL(psi_io_leave);

/* ── Stall count query (for diagnostics / procfs / scheduler) ───── */

int psi_stall_count(int resource)
{
    if ((unsigned)resource >= PSI_NUM_RESOURCES)
        return -1;
    uint64_t flags;
    spinlock_irqsave_acquire(&psi_resources[resource].lock, &flags);
    int cnt = psi_resources[resource].stall_count;
    spinlock_irqsave_release(&psi_resources[resource].lock, flags);
    return cnt;
}

EXPORT_SYMBOL(psi_stall_count);

/* ═══════════════════════════════════════════════════════════════════════
 *  Periodic update timer
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Called every 2 seconds (200 ticks at 100 Hz) to update the PSI moving
 * averages.  Uses the current stall counts to determine whether the
 * resource was in "some" or "full" stall during the window.
 */

/* Timer interval: 2 seconds expressed in timer ticks (TIMER_FREQ = 100 Hz) */
#define PSI_UPDATE_INTERVAL_TICKS  (2 * TIMER_FREQ)   /* 200 ticks */

static int psi_timer_id = -1;

static void psi_timer_cb(void *arg)
{
    (void)arg;
    uint64_t now = timer_get_ticks();

    for (int r = 0; r < PSI_NUM_RESOURCES; r++) {
        struct psi_resource *pr = &psi_resources[r];
        if (!pr->initialized)
            continue;

        uint64_t elapsed = now - pr->last_ticks;
        if (elapsed == 0)
            continue;

        /* Determine stall state from current count.
         * "some" = at least one task stalled on this resource.
         * "full" = all non-idle tasks stalled (simplified: same as some). */
        int stalled = 0;
        uint64_t flags;
        spinlock_irqsave_acquire(&pr->lock, &flags);
        stalled = pr->stall_count;
        spinlock_irqsave_release(&pr->lock, flags);

        uint64_t some_ticks = (stalled > 0) ? elapsed : 0;
        uint64_t full_ticks = 0;
        if (r == PSI_RES_MEMORY || r == PSI_RES_IO)
            full_ticks = (stalled > 0) ? elapsed : 0;

        psi_update(r, elapsed, some_ticks, full_ticks);
    }

    /* Re-schedule for the next 2-second interval */
    psi_timer_id = timer_schedule(psi_timer_cb, NULL, PSI_UPDATE_INTERVAL_TICKS);
}

void psi_timer_init(void)
{
    psi_timer_id = timer_schedule(psi_timer_cb, NULL, PSI_UPDATE_INTERVAL_TICKS);
    if (psi_timer_id < 0)
        kprintf("[!!] PSI: failed to register update timer\n");
    else
        kprintf("[OK] PSI: update timer registered (every %d ticks = 2s)\n",
                PSI_UPDATE_INTERVAL_TICKS);
}

/* ── Stub: psi_trigger ─────────────────────────────── */
static int psi_trigger(const char *event, int threshold, int win_size)
{
    (void)event;
    (void)threshold;
    (void)win_size;
    kprintf("[psi] psi_trigger: not yet implemented\n");
    return 0;
}
/* ── Stub: psi_poll ─────────────────────────────── */
static int psi_poll(void)
{
    kprintf("[psi] psi_poll: not yet implemented\n");
    return 0;
}
