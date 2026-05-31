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

#endif
