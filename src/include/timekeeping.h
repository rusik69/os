#ifndef TIMEKEEPING_H
#define TIMEKEEPING_H

#include "types.h"

/*
 * timekeeping.h — kernel timekeeping: NTP clock adjustment interface
 *
 * The realtime (wall) clock is defined as the boot epoch stored by the RTC
 * driver (rtc_get_epoch/rtc_set_epoch) plus ticks elapsed since boot:
 *
 *     CLOCK_REALTIME = boot_epoch + ticks / TIMER_FREQ
 *
 * NTP disciplines this clock through two adjustment primitives:
 *   1. A hard step to an absolute UTC epoch (timekeeping_ntp_settime),
 *      used when the NTP client first acquires authoritative time.
 *   2. A small slewed offset (in nanoseconds) folded into the reported
 *      CLOCK_REALTIME value without a discontiguous clock jump
 *      (timekeeping_set_rt_offset / timekeeping_get_rt_offset).
 */

/* Hard-step the realtime clock to an absolute UTC epoch (seconds).
 * Resets any previously accumulated slew offset. */
void timekeeping_ntp_settime(uint64_t epoch_sec);

/* Apply a signed slew offset (in ns) to the reported CLOCK_REALTIME. */
void timekeeping_set_rt_offset(int64_t offset_ns);

/* Return the currently applied realtime slew offset (ns). */
int64_t timekeeping_get_rt_offset(void);

/* ── Leap second handling ────────────────────────────────────────── */

/* Return the TAI-UTC offset (in seconds) in effect at UTC epoch @epoch_sec.
 * Derived from the IERS leap-second table.  Returns 0 before the first
 * (1972) leap second. */
int timekeeping_leap_offset(uint64_t epoch_sec);

/* Return the TAI-UTC offset in effect for the current wall-clock time. */
int timekeeping_leap_now(void);

/* ── Coarse vs fine-grained time ────────────────────────────────── */

/* Called from the timer tick (typically on the coarse jiffy boundary).
 * Refreshes the cached coarse realtime / monotonic timestamps. */
void timekeeping_tick_coarse(void);

/* Return the coarse-grained realtime (wall-clock) clock, cached at the
 * last timer tick.  Resolution is one tick period; cheaper than a fresh
 * fine-grained read.  Returns 0 on success. */
int timekeeping_coarse_realtime(struct timespec *ts);

/* Return the coarse-grained monotonic (time-since-boot) clock, cached at
 * the last timer tick.  Returns 0 on success. */
int timekeeping_coarse_monotonic(struct timespec *ts);

#endif /* TIMEKEEPING_H */