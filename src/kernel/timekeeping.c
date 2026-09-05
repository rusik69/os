/*
 * timekeeping.c — kernel timekeeping: NTP clock adjustment
 *
 * Provides the interface the NTP client uses to discipline the realtime
 * (wall) clock.  The base clock source lives in drivers/rtc.c (boot epoch)
 * and posix_timer.c (CLOCK_REALTIME computation); this module supplies the
 * NTP adjustment primitives (absolute step + slewed offset) on top of it.
 */

#define KERNEL_INTERNAL
#include "timekeeping.h"

#include "rtc.h"
#include "timer.h"

/* Slewed offset (ns) applied to the reported CLOCK_REALTIME.  Kept as a
 * volatile so NTP updates are visible to readers on the same core without
 * locking; single-writer/single-reader for this scalar is safe here. */
static volatile int64_t g_rt_offset_ns = 0;

void timekeeping_set_rt_offset(int64_t offset_ns) {
    g_rt_offset_ns = offset_ns;
}

int64_t timekeeping_get_rt_offset(void) {
    return g_rt_offset_ns;
}

void timekeeping_ntp_settime(uint64_t epoch_sec) {
    uint64_t ticks = timer_get_ticks();
    uint64_t ticks_sec = ticks / TIMER_FREQ;
    uint64_t new_epoch;

    /* Rebase the boot epoch so that boot_epoch + ticks == desired time.
     * Clamp to zero if the desired time predates boot. */
    new_epoch = (epoch_sec >= ticks_sec) ? (epoch_sec - ticks_sec) : 0;
    rtc_set_epoch(new_epoch);

    /* A hard step supersedes any accumulated slew offset. */
    g_rt_offset_ns = 0;
}

/* ── Leap second handling ──────────────────────────────────────────
 *
 * TAI-UTC offset is applied instantaneously by the IERS at announced
 * dates.  The table below records, for each leap insertion, the UTC
 * epoch at which the new offset came into effect and the resulting
 * cumulative TAI-UTC offset.  Entries are in strictly ascending epoch
 * order, enabling a simple linear scan.
 */
struct leap_second {
    uint64_t epoch; /* UTC epoch (s) at which the offset steps */
    int offset;     /* cumulative TAI - UTC offset in effect (s) */
};

static const struct leap_second leap_seconds[] = {
    {63072000, 10},   /* 1972-01-01 */
    {78796800, 11},   /* 1972-07-01 */
    {94694400, 12},   /* 1973-01-01 */
    {126230400, 13},  /* 1974-01-01 */
    {157766400, 14},  /* 1975-01-01 */
    {189302400, 15},  /* 1976-01-01 */
    {220924800, 16},  /* 1977-01-01 */
    {252460800, 17},  /* 1978-01-01 */
    {283996800, 18},  /* 1979-01-01 */
    {315532800, 19},  /* 1980-01-01 */
    {362793600, 20},  /* 1981-07-01 */
    {394329600, 21},  /* 1982-07-01 */
    {425865600, 22},  /* 1983-07-01 */
    {489024000, 23},  /* 1985-07-01 */
    {567993600, 24},  /* 1988-01-01 */
    {631152000, 25},  /* 1990-01-01 */
    {662688000, 26},  /* 1991-01-01 */
    {709948800, 27},  /* 1992-07-01 */
    {741484800, 28},  /* 1993-07-01 */
    {773020800, 29},  /* 1994-07-01 */
    {820454400, 30},  /* 1996-01-01 */
    {867715200, 31},  /* 1997-07-01 */
    {915148800, 32},  /* 1999-01-01 */
    {1136073600, 33}, /* 2006-01-01 */
    {1230768000, 34}, /* 2009-01-01 */
    {1341100800, 35}, /* 2012-07-01 */
    {1435708800, 36}, /* 2015-07-01 */
    {1483228800, 37}, /* 2017-01-01 (current) */
};
#define LEAP_SECONDS_N ((int)(sizeof(leap_seconds) / sizeof(leap_seconds[0])))

int timekeeping_leap_offset(uint64_t epoch_sec) {
    int offset = 0;
    for (int i = 0; i < LEAP_SECONDS_N; i++) {
        if (epoch_sec >= leap_seconds[i].epoch)
            offset = leap_seconds[i].offset;
        else
            break; /* table is strictly ascending */
    }
    return offset;
}

int timekeeping_leap_now(void) {
    uint64_t ticks = timer_get_ticks();
    uint64_t epoch = rtc_get_epoch() + (ticks / TIMER_FREQ);
    return timekeeping_leap_offset(epoch);
}

/* ── Coarse vs fine-grained time ────────────────────────────────────
 *
 * Fine-grained clocks (CLOCK_REALTIME / CLOCK_MONOTONIC) recompute the
 * time on every read from the raw tick counter, folding in the sub-tick
 * nanosecond fraction.  The coarse clocks (CLOCK_REALTIME_COARSE /
 * CLOCK_MONOTONIC_COARSE) instead return a snapshot that is updated once
 * per timer tick.  This gives tick-period resolution but avoids the per-
 * call cost of a fresh fine-grained computation — the same trade-off
 * Linux makes with ktime_get_coarse_real_time() vs ktime_get_real().
 *
 * The snapshots are written by the timer tick (interrupt context on one
 * CPU) and read by syscalls; a single aligned 64-bit load/store per field
 * makes them race-safe without locking for the typical uniprocessor or
 * preempt-disabled read path.
 */
static struct {
    int64_t real_sec;
    int64_t real_nsec;
    int64_t mono_sec;
    int64_t mono_nsec;
} g_coarse;

void timekeeping_tick_coarse(void) {
    uint64_t ticks = timer_get_ticks();
    uint64_t epoch = rtc_get_epoch();
    int64_t off = timekeeping_get_rt_offset();
    int64_t real_sec = (int64_t)(epoch + (ticks / TIMER_FREQ));

    /* Fold the NTP slew offset into the coarse realtime snapshot,
     * carrying into the seconds field to keep nsec in range. */
    int64_t ns = (int64_t)((ticks % TIMER_FREQ) * NS_PER_TICK) + off;
    while (ns >= 1000000000LL) {
        real_sec += 1;
        ns -= 1000000000LL;
    }
    while (ns < 0) {
        real_sec -= 1;
        ns += 1000000000LL;
    }

    g_coarse.real_sec = real_sec;
    g_coarse.real_nsec = ns;
    g_coarse.mono_sec = (int64_t)(ticks / TIMER_FREQ);
    g_coarse.mono_nsec = (int64_t)((ticks % TIMER_FREQ) * NS_PER_TICK);
}

int timekeeping_coarse_realtime(struct timespec *ts) {
    ts->tv_sec = (uint64_t)g_coarse.real_sec;
    ts->tv_nsec = (uint64_t)g_coarse.real_nsec;
    return 0;
}

int timekeeping_coarse_monotonic(struct timespec *ts) {
    ts->tv_sec = (uint64_t)g_coarse.mono_sec;
    ts->tv_nsec = (uint64_t)g_coarse.mono_nsec;
    return 0;
}