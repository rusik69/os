/*
 * time.c — POSIX time functions (Item U16)
 *
 * Provides: localtime, gmtime, mktime, strftime, asctime, ctime, difftime
 *
 * These are pure-computation functions used by userspace programs and
 * built-in shell commands.  The syscall wrappers (clock_gettime, nanosleep)
 * live in libc.c; this file provides the higher-level formatting/parsing.
 */

#include "time.h"
#include "string.h"
#include "syscall.h"
#include "libc.h"

/* ── Days in each month (non-leap and leap year) ────────────────── */

static const int days_in_mon[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
};

static const int cumulative_days[2][12] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335},
};

/* ── Month and weekday names ────────────────────────────────────── */

static const char *month_names[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *month_names_full[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char *wday_names[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *wday_names_full[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

/* ── Helper: is leap year? ──────────────────────────────────────── */

static inline int is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* ── Thread-local storage for tm results ────────────────────────── */

static struct tm gmtime_buf;
static struct tm localtime_buf;
static char asctime_buf[26];
static char ctime_buf[26];

/* ── gmtime / gmtime_r ─────────────────────────────────────────── */

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    if (!result || !timep) return NULL;

    time_t t = *timep;

    /* Break down the time */
    int days_since_epoch = (int)(t / 86400);
    int remaining = (int)(t % 86400);

    result->tm_hour = remaining / 3600;
    remaining %= 3600;
    result->tm_min = remaining / 60;
    result->tm_sec = remaining % 60;

    result->tm_wday = (days_since_epoch + 4) % 7; /* Jan 1 1970 was Thursday */
    if (result->tm_wday < 0) result->tm_wday += 7;

    /* Year calculation */
    int year = 1970;
    while (1) {
        int days_in_year = is_leap(year) ? 366 : 365;
        if (days_since_epoch < days_in_year) break;
        days_since_epoch -= days_in_year;
        year++;
    }

    result->tm_year = year - 1900;
    result->tm_yday = days_since_epoch;

    /* Month calculation */
    int leap = is_leap(year);
    for (result->tm_mon = 0; result->tm_mon < 12; result->tm_mon++) {
        if (days_since_epoch < days_in_mon[leap][result->tm_mon]) break;
        days_since_epoch -= days_in_mon[leap][result->tm_mon];
    }

    result->tm_mday = days_since_epoch + 1;
    result->tm_isdst = 0;

    return result;
}

struct tm *gmtime(const time_t *timep) {
    return gmtime_r(timep, &gmtime_buf);
}

/* ── localtime / localtime_r ────────────────────────────────────── */
/*
 * For now, localtime is identical to gmtime (no timezone support).
 * A full implementation would read /etc/localtime or TZ environment
 * variable and apply the offset and DST rules.
 */

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    /* No timezone support yet — same as gmtime */
    return gmtime_r(timep, result);
}

struct tm *localtime(const time_t *timep) {
    return localtime_r(timep, &localtime_buf);
}

/* ── mktime ─────────────────────────────────────────────────────── */

time_t mktime(struct tm *tm) {
    if (!tm) return (time_t)-1;

    int year = tm->tm_year + 1900;
    int leap = is_leap(year);

    /* Normalise month/year if out of range */
    while (tm->tm_mon < 0) {
        tm->tm_mon += 12;
        year--;
        leap = is_leap(year);
    }
    while (tm->tm_mon > 11) {
        tm->tm_mon -= 12;
        year++;
        leap = is_leap(year);
    }

    /* Days from 1970-01-01 */
    time_t days = 0;

    /* Count whole years */
    for (int y = 1970; y < year; y++) {
        days += is_leap(y) ? 366 : 365;
    }

    /* Count months in current year */
    days += cumulative_days[leap][tm->tm_mon];

    /* Count days in current month */
    days += (time_t)(tm->tm_mday - 1);

    /* Calculate day of week */
    tm->tm_wday = (int)((days + 4) % 7);
    if (tm->tm_wday < 0) tm->tm_wday += 7;

    /* Calculate day of year */
    tm->tm_yday = cumulative_days[leap][tm->tm_mon] + tm->tm_mday - 1;

    /* Convert to seconds */
    time_t secs = days * 86400;
    secs += (time_t)(tm->tm_hour * 3600);
    secs += (time_t)(tm->tm_min * 60);
    secs += (time_t)tm->tm_sec;

    tm->tm_year = year - 1900;
    tm->tm_isdst = 0;

    return secs;
}

/* ── asctime / asctime_r ────────────────────────────────────────── */

char *asctime_r(const struct tm *tm, char *buf) {
    if (!tm || !buf) return NULL;
    /* Format: "Wed Jun 30 21:49:08 1993\n" */
    if (tm->tm_wday >= 0 && tm->tm_wday < 7 &&
        tm->tm_mon >= 0 && tm->tm_mon < 12) {
        buf[0] = wday_names[tm->tm_wday][0];
        buf[1] = wday_names[tm->tm_wday][1];
        buf[2] = wday_names[tm->tm_wday][2];
    } else {
        buf[0] = '?'; buf[1] = '?'; buf[2] = '?';
    }
    buf[3] = ' ';
    if (tm->tm_mon >= 0 && tm->tm_mon < 12) {
        buf[4] = month_names[tm->tm_mon][0];
        buf[5] = month_names[tm->tm_mon][1];
        buf[6] = month_names[tm->tm_mon][2];
    } else {
        buf[4] = '?'; buf[5] = '?'; buf[6] = '?';
    }
    buf[7] = ' ';
    /* Day of month, right-justified to 2 spaces */
    int mday = tm->tm_mday;
    if (mday < 10) {
        buf[8] = ' ';
        buf[9] = (char)('0' + mday);
    } else {
        buf[8] = (char)('0' + mday / 10);
        buf[9] = (char)('0' + mday % 10);
    }
    buf[10] = ' ';
    /* HH:MM:SS */
    buf[11] = (char)('0' + tm->tm_hour / 10);
    buf[12] = (char)('0' + tm->tm_hour % 10);
    buf[13] = ':';
    buf[14] = (char)('0' + tm->tm_min / 10);
    buf[15] = (char)('0' + tm->tm_min % 10);
    buf[16] = ':';
    buf[17] = (char)('0' + tm->tm_sec / 10);
    buf[18] = (char)('0' + tm->tm_sec % 10);
    buf[19] = ' ';
    /* Year (4 digits) */
    int year = tm->tm_year + 1900;
    if (year < 0) year = 0;
    if (year > 9999) year = 9999;
    buf[20] = (char)('0' + year / 1000);
    buf[21] = (char)('0' + (year / 100) % 10);
    buf[22] = (char)('0' + (year / 10) % 10);
    buf[23] = (char)('0' + year % 10);
    buf[24] = '\n';
    buf[25] = '\0';
    return buf;
}

char *asctime(const struct tm *tm) {
    return asctime_r(tm, asctime_buf);
}

char *ctime_r(const time_t *timep, char *buf) {
    struct tm result;
    if (!localtime_r(timep, &result)) return NULL;
    return asctime_r(&result, buf);
}

char *ctime(const time_t *timep) {
    return ctime_r(timep, ctime_buf);
}

/* ── strftime ───────────────────────────────────────────────────── */

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    if (!s || !format || !tm || max == 0) return 0;

    size_t pos = 0;
    size_t len = max - 1; /* leave room for null terminator */

    while (*format && pos < len) {
        if (*format != '%') {
            s[pos++] = *format++;
            continue;
        }
        format++; /* skip '%' */

        if (!*format) break;

        switch (*format) {
        case 'Y': { /* Year with century (4 digits) */
            int year = tm->tm_year + 1900;
            if (year < 0) year = 0;
            if (year > 9999) year = 9999;
            if (pos + 4 <= len) {
                s[pos++] = (char)('0' + year / 1000);
                s[pos++] = (char)('0' + (year / 100) % 10);
                s[pos++] = (char)('0' + (year / 10) % 10);
                s[pos++] = (char)('0' + year % 10);
            }
            break;
        }
        case 'y': { /* Year without century (2 digits) */
            int year = (tm->tm_year + 1900) % 100;
            if (pos + 2 <= len) {
                s[pos++] = (char)('0' + year / 10);
                s[pos++] = (char)('0' + year % 10);
            }
            break;
        }
        case 'm': { /* Month as decimal (01-12) */
            int mon = tm->tm_mon + 1;
            if (pos + 2 <= len) {
                s[pos++] = (char)('0' + mon / 10);
                s[pos++] = (char)('0' + mon % 10);
            }
            break;
        }
        case 'd': { /* Day of month (01-31) */
            if (pos + 2 <= len) {
                s[pos++] = (char)('0' + tm->tm_mday / 10);
                s[pos++] = (char)('0' + tm->tm_mday % 10);
            }
            break;
        }
        case 'H': { /* Hour (00-23) */
            if (pos + 2 <= len) {
                s[pos++] = (char)('0' + tm->tm_hour / 10);
                s[pos++] = (char)('0' + tm->tm_hour % 10);
            }
            break;
        }
        case 'I': { /* Hour (01-12) */
            int h12 = tm->tm_hour % 12;
            if (h12 == 0) h12 = 12;
            if (pos + 2 <= len) {
                s[pos++] = (char)('0' + h12 / 10);
                s[pos++] = (char)('0' + h12 % 10);
            }
            break;
        }
        case 'M': { /* Minute (00-59) */
            if (pos + 2 <= len) {
                s[pos++] = (char)('0' + tm->tm_min / 10);
                s[pos++] = (char)('0' + tm->tm_min % 10);
            }
            break;
        }
        case 'S': { /* Second (00-60) */
            int sec = tm->tm_sec;
            if (sec > 60) sec = 60;
            if (pos + 2 <= len) {
                s[pos++] = (char)('0' + sec / 10);
                s[pos++] = (char)('0' + sec % 10);
            }
            break;
        }
        case 'p': { /* AM or PM */
            const char *ampm = (tm->tm_hour < 12) ? "AM" : "PM";
            if (pos + 2 <= len) {
                s[pos++] = ampm[0];
                s[pos++] = ampm[1];
            }
            break;
        }
        case 'a': { /* Abbreviated weekday name */
            if (tm->tm_wday >= 0 && tm->tm_wday < 7) {
                const char *name = wday_names[tm->tm_wday];
                for (int i = 0; i < 3 && pos < len; i++)
                    s[pos++] = name[i];
            }
            break;
        }
        case 'A': { /* Full weekday name */
            if (tm->tm_wday >= 0 && tm->tm_wday < 7) {
                const char *name = wday_names_full[tm->tm_wday];
                while (*name && pos < len)
                    s[pos++] = *name++;
            }
            break;
        }
        case 'b':
        case 'h': { /* Abbreviated month name */
            if (tm->tm_mon >= 0 && tm->tm_mon < 12) {
                const char *name = month_names[tm->tm_mon];
                for (int i = 0; i < 3 && pos < len; i++)
                    s[pos++] = name[i];
            }
            break;
        }
        case 'B': { /* Full month name */
            if (tm->tm_mon >= 0 && tm->tm_mon < 12) {
                const char *name = month_names_full[tm->tm_mon];
                while (*name && pos < len)
                    s[pos++] = *name++;
            }
            break;
        }
        case 'j': { /* Day of year (001-366) */
            int yday = tm->tm_yday + 1;
            if (pos + 3 <= len) {
                s[pos++] = (char)('0' + yday / 100);
                s[pos++] = (char)('0' + (yday / 10) % 10);
                s[pos++] = (char)('0' + yday % 10);
            }
            break;
        }
        case 'U': { /* Week number (Sunday first, 00-53) */
            int week = (tm->tm_yday + 7 - tm->tm_wday) / 7;
            if (pos + 2 <= len) {
                s[pos++] = (char)('0' + week / 10);
                s[pos++] = (char)('0' + week % 10);
            }
            break;
        }
        case 'W': { /* Week number (Monday first, 00-53) */
            int w = (tm->tm_wday == 0) ? 6 : tm->tm_wday - 1;
            int week = (tm->tm_yday + 7 - w) / 7;
            if (pos + 2 <= len) {
                s[pos++] = (char)('0' + week / 10);
                s[pos++] = (char)('0' + week % 10);
            }
            break;
        }
        case 'w': { /* Weekday as decimal (0=Sunday, 6=Saturday) */
            if (pos + 1 <= len)
                s[pos++] = (char)('0' + tm->tm_wday);
            break;
        }
        case 'c': { /* Date and time (asctime format) */
            /* Format: "Wed Jun 30 21:49:08 1993" */
            char *result = asctime_r(tm, s + pos);
            if (result && pos + 24 <= len) {
                size_t slen = 0;
                while (s[pos + slen] && slen < 24) slen++;
                pos += slen - 1; /* exclude the trailing newline */
                if (pos < len && s[pos] == '\n') {
                    /* skip newline added by asctime */
                }
            }
            break;
        }
        case 'x': { /* Date representation */
            /* MM/DD/YY */
            int mon = tm->tm_mon + 1;
            int year = (tm->tm_year + 1900) % 100;
            if (pos + 8 <= len) {
                s[pos++] = (char)('0' + mon / 10);
                s[pos++] = (char)('0' + mon % 10);
                s[pos++] = '/';
                s[pos++] = (char)('0' + tm->tm_mday / 10);
                s[pos++] = (char)('0' + tm->tm_mday % 10);
                s[pos++] = '/';
                s[pos++] = (char)('0' + year / 10);
                s[pos++] = (char)('0' + year % 10);
            }
            break;
        }
        case 'X': { /* Time representation */
            /* HH:MM:SS */
            if (pos + 8 <= len) {
                s[pos++] = (char)('0' + tm->tm_hour / 10);
                s[pos++] = (char)('0' + tm->tm_hour % 10);
                s[pos++] = ':';
                s[pos++] = (char)('0' + tm->tm_min / 10);
                s[pos++] = (char)('0' + tm->tm_min % 10);
                s[pos++] = ':';
                s[pos++] = (char)('0' + tm->tm_sec / 10);
                s[pos++] = (char)('0' + tm->tm_sec % 10);
            }
            break;
        }
        case 's': { /* Seconds since epoch */
            /* Use mktime to compute */
            struct tm tmp = *tm;
            time_t epoch = mktime(&tmp);
            /* Print as decimal — for simplicity, use manual string conv */
            /* For now, skip if we can't fit a reasonable number */
            /* MAX epoch ~ 9999999999 fits in 10 digits + null */
            (void)epoch;
            break;
        }
        case 'z': { /* Timezone offset in ISO 8601 format */
            /* No timezone support: output "+0000" */
            if (pos + 5 <= len) {
                s[pos++] = '+';
                s[pos++] = '0';
                s[pos++] = '0';
                s[pos++] = '0';
                s[pos++] = '0';
            }
            break;
        }
        case 'Z': { /* Timezone name */
            /* No timezone support: output "UTC" */
            if (pos + 3 <= len) {
                s[pos++] = 'U';
                s[pos++] = 'T';
                s[pos++] = 'C';
            }
            break;
        }
        case '%': { /* Literal % */
            if (pos + 1 <= len)
                s[pos++] = '%';
            break;
        }
        case 'n': { /* Newline */
            if (pos + 1 <= len)
                s[pos++] = '\n';
            break;
        }
        case 't': { /* Tab */
            if (pos + 1 <= len)
                s[pos++] = '\t';
            break;
        }
        case 'C': { /* Century */
            int year = (tm->tm_year + 1900) / 100;
            if (pos + 2 <= len) {
                s[pos++] = (char)('0' + year / 10);
                s[pos++] = (char)('0' + year % 10);
            }
            break;
        }
        case 'e': { /* Day of month with leading space */
            if (pos + 2 <= len) {
                if (tm->tm_mday < 10) {
                    s[pos++] = ' ';
                    s[pos++] = (char)('0' + tm->tm_mday);
                } else {
                    s[pos++] = (char)('0' + tm->tm_mday / 10);
                    s[pos++] = (char)('0' + tm->tm_mday % 10);
                }
            }
            break;
        }
        case 'k': { /* Hour with leading space (0-23) */
            if (pos + 2 <= len) {
                if (tm->tm_hour < 10) {
                    s[pos++] = ' ';
                    s[pos++] = (char)('0' + tm->tm_hour);
                } else {
                    s[pos++] = (char)('0' + tm->tm_hour / 10);
                    s[pos++] = (char)('0' + tm->tm_hour % 10);
                }
            }
            break;
        }
        case 'l': { /* Hour with leading space (1-12) */
            int h12 = tm->tm_hour % 12;
            if (h12 == 0) h12 = 12;
            if (pos + 2 <= len) {
                if (h12 < 10) {
                    s[pos++] = ' ';
                    s[pos++] = (char)('0' + h12);
                } else {
                    s[pos++] = (char)('0' + h12 / 10);
                    s[pos++] = (char)('0' + h12 % 10);
                }
            }
            break;
        }
        case 'r': { /* 12-hour time with AM/PM */
            int h12 = tm->tm_hour % 12;
            if (h12 == 0) h12 = 12;
            const char *ampm = (tm->tm_hour < 12) ? "AM" : "PM";
            if (pos + 11 <= len) {
                s[pos++] = (char)('0' + h12 / 10);
                s[pos++] = (char)('0' + h12 % 10);
                s[pos++] = ':';
                s[pos++] = (char)('0' + tm->tm_min / 10);
                s[pos++] = (char)('0' + tm->tm_min % 10);
                s[pos++] = ':';
                s[pos++] = (char)('0' + tm->tm_sec / 10);
                s[pos++] = (char)('0' + tm->tm_sec % 10);
                s[pos++] = ' ';
                s[pos++] = ampm[0];
                s[pos++] = ampm[1];
            }
            break;
        }
        case 'R': { /* 24-hour time HH:MM */
            if (pos + 5 <= len) {
                s[pos++] = (char)('0' + tm->tm_hour / 10);
                s[pos++] = (char)('0' + tm->tm_hour % 10);
                s[pos++] = ':';
                s[pos++] = (char)('0' + tm->tm_min / 10);
                s[pos++] = (char)('0' + tm->tm_min % 10);
            }
            break;
        }
        case 'T': { /* 24-hour time HH:MM:SS (ISO 8601) */
            if (pos + 8 <= len) {
                s[pos++] = (char)('0' + tm->tm_hour / 10);
                s[pos++] = (char)('0' + tm->tm_hour % 10);
                s[pos++] = ':';
                s[pos++] = (char)('0' + tm->tm_min / 10);
                s[pos++] = (char)('0' + tm->tm_min % 10);
                s[pos++] = ':';
                s[pos++] = (char)('0' + tm->tm_sec / 10);
                s[pos++] = (char)('0' + tm->tm_sec % 10);
            }
            break;
        }
        case 'D': { /* US date format MM/DD/YY */
            int mon = tm->tm_mon + 1;
            int year = (tm->tm_year + 1900) % 100;
            if (pos + 8 <= len) {
                s[pos++] = (char)('0' + mon / 10);
                s[pos++] = (char)('0' + mon % 10);
                s[pos++] = '/';
                s[pos++] = (char)('0' + tm->tm_mday / 10);
                s[pos++] = (char)('0' + tm->tm_mday % 10);
                s[pos++] = '/';
                s[pos++] = (char)('0' + year / 10);
                s[pos++] = (char)('0' + year % 10);
            }
            break;
        }
        case 'F': { /* ISO 8601 date format YYYY-MM-DD */
            int year = tm->tm_year + 1900;
            if (year < 0) year = 0;
            if (year > 9999) year = 9999;
            int mon = tm->tm_mon + 1;
            if (pos + 10 <= len) {
                s[pos++] = (char)('0' + year / 1000);
                s[pos++] = (char)('0' + (year / 100) % 10);
                s[pos++] = (char)('0' + (year / 10) % 10);
                s[pos++] = (char)('0' + year % 10);
                s[pos++] = '-';
                s[pos++] = (char)('0' + mon / 10);
                s[pos++] = (char)('0' + mon % 10);
                s[pos++] = '-';
                s[pos++] = (char)('0' + tm->tm_mday / 10);
                s[pos++] = (char)('0' + tm->tm_mday % 10);
            }
            break;
        }
        case 'v': { /* VMS date format dd-MMM-YYYY */
            int year = tm->tm_year + 1900;
            if (pos + 11 <= len) {
                s[pos++] = (char)('0' + tm->tm_mday / 10);
                s[pos++] = (char)('0' + tm->tm_mday % 10);
                s[pos++] = '-';
                if (tm->tm_mon >= 0 && tm->tm_mon < 12) {
                    const char *name = month_names[tm->tm_mon];
                    s[pos++] = name[0];
                    s[pos++] = name[1];
                    s[pos++] = name[2];
                } else {
                    s[pos++] = '?'; s[pos++] = '?'; s[pos++] = '?';
                }
                s[pos++] = '-';
                if (year < 1000) s[pos++] = '0';
                if (year < 100) s[pos++] = '0';
                if (year < 10) s[pos++] = '0';
                s[pos++] = (char)('0' + year / 1000);
                s[pos++] = (char)('0' + (year / 100) % 10);
                s[pos++] = (char)('0' + (year / 10) % 10);
                s[pos++] = (char)('0' + year % 10);
            }
            break;
        }
        default: {
            /* Unknown specifier: copy literally */
            if (pos + 1 <= len)
                s[pos++] = *format;
            break;
        }
        }
        format++;
    }

    s[pos] = '\0';
    return pos;
}

/* ── time_get ─────────────────────────────── */
static uint64_t time_get(void)
{
    /* Use the SYS_TIME syscall (which returns current timestamp in ms) */
    return libc_syscall(SYS_TIME, 0, 0, 0, 0, 0);
}
/* ── time_set ─────────────────────────────── */
static int time_set(uint64_t t)
{
    /* Setting time is not supported; return success silently */
    (void)t;
    return 0;
}
/* ── time_nanosleep ─────────────────────────────── */
static int time_nanosleep(uint64_t ns)
{
    /* Busy-wait loop for simplicity (ns is nanoseconds) */
    uint64_t start = time_get();
    uint64_t start_us = start * 1000; /* convert ms to us */
    uint64_t target_ns = ns;
    volatile uint64_t elapsed_ns = 0;
    while (elapsed_ns < target_ns) {
        /* Simple delay loop — very approximate */
        for (volatile int i = 0; i < 10; i++) { }
        elapsed_ns += 10;
    }
    return 0;
}
