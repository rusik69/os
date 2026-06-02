#ifndef TIME_H
#define TIME_H

#include "types.h"
#include "libc.h"

/* ── Time types ─────────────────────────────────────────────────── */

typedef uint64_t time_t;
typedef uint64_t clockid_t;
typedef int64_t  suseconds_t;

/* ── Clock identifiers ──────────────────────────────────────────── */

#define CLOCK_REALTIME                 0
#define CLOCK_MONOTONIC                1
#define CLOCK_PROCESS_CPUTIME_ID       2
#define CLOCK_THREAD_CPUTIME_ID        3
#define CLOCK_MONOTONIC_RAW            4
#define CLOCK_REALTIME_COARSE          5
#define CLOCK_MONOTONIC_COARSE         6
#define CLOCK_BOOTTIME                 7
#define CLOCK_REALTIME_ALARM           8
#define CLOCK_BOOTTIME_ALARM           9

/* ── Broken-down time structure ─────────────────────────────────── */

struct tm {
    int tm_sec;    /* seconds (0-60) */
    int tm_min;    /* minutes (0-59) */
    int tm_hour;   /* hours (0-23) */
    int tm_mday;   /* day of month (1-31) */
    int tm_mon;    /* month (0-11) */
    int tm_year;   /* year - 1900 */
    int tm_wday;   /* day of week (0-6, Sunday = 0) */
    int tm_yday;   /* day of year (0-365) */
    int tm_isdst;  /* daylight saving time flag */
};

/* ── Itimerspec for timer_settime ───────────────────────────────── */

struct itimerspec {
    struct timespec it_interval;  /* timer period */
    struct timespec it_value;     /* timer expiration */
};

/* ── Timezone structure ───────────────────────────────────────── */

struct timezone {
    int tz_minuteswest;  /* minutes west of Greenwich */
    int tz_dsttime;      /* type of DST correction */
};

/* ── Inline POSIX wrappers (call libc_* implementations) ────────── */

static inline int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    return libc_clock_gettime((uint64_t)clk_id, tp);
}

static inline int clock_settime(clockid_t clk_id, const struct timespec *tp) {
    return libc_clock_settime((uint64_t)clk_id, tp);
}

static inline int nanosleep(const struct timespec *req, struct timespec *rem) {
    return libc_nanosleep(req, rem);
}

static inline time_t time(time_t *t) {
    time_t ret = libc_syscall(11, (uint64_t)(uintptr_t)t, 0, 0, 0, 0);
    return ret;
}

static inline int gettimeofday(struct timeval *tv, struct timezone *tz) {
    struct timespec ts;
    int ret = clock_gettime(CLOCK_REALTIME, &ts);
    if (ret == 0 && tv) {
        tv->tv_sec = ts.tv_sec;
        tv->tv_usec = (uint64_t)(ts.tv_nsec / 1000);
    }
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return ret;
}

/* ── Standard library time functions ─────────────────────────────── */

/* Convert time_t to broken-down time (UTC) */
struct tm *gmtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);

/* Convert time_t to broken-down time (local time) */
struct tm *localtime(const time_t *timep);
struct tm *localtime_r(const time_t *timep, struct tm *result);

/* Convert broken-down time to time_t */
time_t mktime(struct tm *tm);

/* Format a time string */
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

/* Simple time string representations */
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime(const time_t *timep);
char *ctime_r(const time_t *timep, char *buf);

/* Difference between two times */
static inline time_t difftime(time_t time1, time_t time0) {
    return time1 - time0;
}

#endif /* TIME_H */
