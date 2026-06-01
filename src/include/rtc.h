#ifndef RTC_H
#define RTC_H

#include "types.h"

/* Calendar time structure from RTC */
struct rtc_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

void rtc_init(void);
void rtc_get_time(struct rtc_time *t);

/* Wall clock (Unix epoch) support.
 * boot_epoch stores the Unix epoch time at boot (in seconds).
 * clock_gettime(CLOCK_REALTIME) returns boot_epoch + ticks_elapsed.
 * clock_settime adjusts boot_epoch. */
uint64_t rtc_get_epoch(void);       /* current wall clock epoch seconds */
void     rtc_set_epoch(uint64_t s); /* set wall clock epoch (for settime) */

/* Convert an rtc_time struct to Unix epoch seconds (days since 1970).
 * Exposed so syscall.c can compute from raw RTC time if needed. */
uint64_t rtc_to_epoch(const struct rtc_time *t);

/* ── Periodic interrupt ────────────────────────────────────────────── */

/* Enable or disable the RTC periodic interrupt at the given rate.
   rate_hz must be a power-of-2 divisor of 32768 (2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192).
   Returns 0 on success, -1 on invalid rate. */
int rtc_set_periodic(int enable, int rate_hz);

/* Wait for a given number of periodic ticks (blocking).
   Returns 0 on success, -1 if periodic wasn't enabled. */
int rtc_wait_ticks(uint32_t ticks);

/* ── Alarm ─────────────────────────────────────────────────────────── */

/* Set the RTC alarm time.
   Only the enabled fields (second, minute, hour, day) are compared;
   set any field to 0xFF to disable matching on that field.
   Returns 0 on success, -1 on error. */
int rtc_set_alarm(const struct rtc_time *t);
int rtc_alarm_enable(int enable);

/* RTC periodic counter */
uint64_t rtc_get_ticks(void);

/* Check if the alarm has fired since last cleared.
   Returns 1 if fired, 0 otherwise. */
int rtc_alarm_fired(void);

/* Clear the alarm-fired flag. */
void rtc_alarm_clear(void);

#endif /* RTC_H */
