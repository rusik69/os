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