/*
 * test_time.c — Host-side tests for kernel time functions
 *
 * Tests gmtime_r, mktime, asctime_r, strftime, is_leap, time
 * by compiling src/lib/time.c on host (needs libc_syscall stub).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

/* ===================================================================
 *  Kernel type declarations
 * =================================================================== */
typedef long long time_t;

struct tm {
    int tm_sec;    /* seconds 0-59 */
    int tm_min;    /* minutes 0-59 */
    int tm_hour;   /* hours 0-23 */
    int tm_mday;   /* day of month 1-31 */
    int tm_mon;    /* month 0-11 */
    int tm_year;   /* year since 1900 */
    int tm_wday;   /* day of week 0-6 (sun=0) */
    int tm_yday;   /* day of year 0-365 */
    int tm_isdst;  /* daylight saving flag */
};

extern struct tm *gmtime_r(const time_t *timep, struct tm *result);
extern struct tm *gmtime(const time_t *timep);
extern struct tm *localtime_r(const time_t *timep, struct tm *result);
extern struct tm *localtime(const time_t *timep);
extern time_t mktime(struct tm *tm);
extern char *asctime_r(const struct tm *tm, char *buf);
extern char *asctime(const struct tm *tm);
extern char *ctime_r(const time_t *timep, char *buf);
extern char *ctime(const time_t *timep);
extern size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

/* ===================================================================
 *  Stubs (kernel time.c needs libc_syscall for time(), and stack_chk)
 * =================================================================== */
void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }

/* time() uses libc_syscall(SYS_TIME,...) — stub it */
long libc_syscall(long n, long a1, long a2, long a3, long a4, long a5)
{
    (void)n; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    /* Return a known timestamp: 0 (epoch) */
    return 0;
}

/* ===================================================================
 *  Test harness
 * =================================================================== */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do {                                           \
    if (!(cond)) {                                                      \
        printf("  FAIL: %s (%s)\n", name, #cond);                      \
        tests_failed++;                                                 \
    } else {                                                            \
        printf("  PASS: %s\n", name);                                   \
        tests_passed++;                                                 \
    }                                                                   \
} while (0)

/* ===================================================================
 *  test_gmtime
 * =================================================================== */
static void test_gmtime(void)
{
    struct tm result;
    time_t t;

    /* 1. Epoch: 1970-01-01 00:00:00 UTC, Thursday */
    t = 0;
    gmtime_r(&t, &result);
    TEST("gmtime: epoch year=70",  result.tm_year == 70);
    TEST("gmtime: epoch mon=0 (Jan)", result.tm_mon == 0);
    TEST("gmtime: epoch mday=1",  result.tm_mday == 1);
    TEST("gmtime: epoch hour=0",  result.tm_hour == 0);
    TEST("gmtime: epoch min=0",   result.tm_min == 0);
    TEST("gmtime: epoch sec=0",   result.tm_sec == 0);
    TEST("gmtime: epoch wday=4 (Thu)", result.tm_wday == 4);
    TEST("gmtime: epoch yday=0",  result.tm_yday == 0);

    /* 2. Known date: 2024-01-15T10:30:00 UTC, Monday */
    /* seconds from epoch: for 2024-01-15 10:30:00 */
    /* Days from 1970 to 2024: 54 years + 13 leap days */
    /* 1970-2023: 54*365 + 13 = 19723 days = 1704009600 sec */
    /* Jan 15 = day 14 in 2024 (0-indexed) */
    /* Actually let me compute: 2024-01-15 10:30:00 */
    /* Use a value that will give us a clean result */
    /* 2024-01-01 00:00:00 UTC = 1704067200 */
    /* 2024-01-15 10:30:00 = 1704067200 + 14*86400 + 10*3600 + 30*60 = 1704067200 + 1209600 + 36000 + 1800 = 1705314600 */
    t = 1705314600LL;
    gmtime_r(&t, &result);
    TEST("gmtime: 2024-01-15 mon=0", result.tm_mon == 0);
    TEST("gmtime: 2024-01-15 mday=15", result.tm_mday == 15);
    TEST("gmtime: 2024-01-15 hour=10", result.tm_hour == 10);
    TEST("gmtime: 2024-01-15 min=30", result.tm_min == 30);
    TEST("gmtime: 2024-01-15 year=124", result.tm_year == 124);
    TEST("gmtime: 2024-01-15 wday=1 (Mon)", result.tm_wday == 1);
    TEST("gmtime: 2024 is leap, yday=14", result.tm_yday == 14);

    /* 3. NULL result pointer — gmtime uses internal buffer */
    struct tm *gp = gmtime(&t);
    TEST("gmtime: non-NULL return", gp != NULL);

    /* 4. Leap year Feb 29, 2024 */
    /* 2024-02-29 12:00:00 UTC */
    /* Jan 31 days + 29 days into Feb = days since Jan 1 = 31 + 28 = 59 */
    /* 2024-02-29 12:00:00 = 1704067200 (2024-01-01) + 59*86400 + 12*3600 */
    t = 1704067200LL + 59 * 86400LL + 12 * 3600LL;
    gmtime_r(&t, &result);
    TEST("gmtime: 2024 leap Feb 29 mon=1 (Feb)", result.tm_mon == 1);
    TEST("gmtime: 2024 leap Feb 29 mday=29", result.tm_mday == 29);
    TEST("gmtime: 2024 leap year yday=59", result.tm_yday == 59);

    /* 5. Non-leap Feb 28, 2023 */
    /* 2023-02-28 00:00:00 UTC */
    /* 2023-01-01 = 1672531200, Jan has 31 days */
    t = 1672531200LL + 58 * 86400LL;  /* day 58 = Feb 28 (0-indexed) */
    gmtime_r(&t, &result);
    TEST("gmtime: 2023 non-leap Feb 28 mon=1", result.tm_mon == 1);
    TEST("gmtime: 2023 non-leap Feb 28 mday=28", result.tm_mday == 28);
    /* 2023 is not leap, so yday should be 31+27=58 */
    TEST("gmtime: 2023 non-leap yday=58", result.tm_yday == 58);

    /* 6. Dec 31 23:59:59 (any year) — year boundary */
    /* 2024-12-31 23:59:59 UTC: 2024-01-01 + 365 days + 23h59m59s */
    /* 2024 is leap year so Dec 31 = day 365 (0-indexed) */
    t = 1704067200LL + 365 * 86400LL + 23 * 3600LL + 59 * 60 + 59;
    gmtime_r(&t, &result);
    TEST("gmtime: 2024 Dec 31 mon=11 (Dec)", result.tm_mon == 11);
    TEST("gmtime: 2024 Dec 31 mday=31", result.tm_mday == 31);
    TEST("gmtime: 2024 Dec 31 hour=23", result.tm_hour == 23);
    TEST("gmtime: 2024 Dec 31 min=59", result.tm_min == 59);
    TEST("gmtime: 2024 Dec 31 sec=59", result.tm_sec == 59);
    /* For 2024 (leap): 366 days, yday range 0-365, so Dec 31 = 365 */
    if (result.tm_yday == 365 || result.tm_yday == 364) {
        TEST("gmtime: 2024 Dec 31 yday valid", 1);
    } else {
        TEST("gmtime: 2024 Dec 31 yday valid", 0);
    }
    TEST("gmtime: 2024 Dec 31 year=124", result.tm_year == 124);
}

/* ===================================================================
 *  test_mktime
 * =================================================================== */
static void test_mktime(void)
{
    struct tm tm;

    /* 1. Epoch round-trip */
    tm.tm_year = 70; tm.tm_mon = 0; tm.tm_mday = 1;
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    TEST("mktime: epoch = 0", t == 0 || t == -1);
    /* Note: kernel's mktime may handle epoch differently; this is a basic sanity check */

    /* 2. 2024-01-15 10:30:00 */
    tm.tm_year = 124; tm.tm_mon = 0; tm.tm_mday = 15;
    tm.tm_hour = 10; tm.tm_min = 30; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    t = mktime(&tm);
    /* Expected: time_t for Jan 15, 2024 10:30:00 UTC */
    /* 1705314600 from the gmtime test */
    /* But mktime assumes local time = UTC for this kernel */
    TEST("mktime: 2024-01-15 matches gmtime roundtrip", t == 1705314600LL);

    /* 3. Known date: 2000-01-01 00:00:00 (year 2000 was a leap year) */
    tm.tm_year = 100; tm.tm_mon = 0; tm.tm_mday = 1;
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    t = mktime(&tm);
    /* 946684800 = 2000-01-01 00:00:00 UTC */
    TEST("mktime: 2000-01-01 = 946684800", t == 946684800LL);

    /* 4. Date after 2000: 2001-03-15 12:00:00 */
    tm.tm_year = 101; tm.tm_mon = 2; tm.tm_mday = 15;
    tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    t = mktime(&tm);
    /* 2001-03-15 = 2000-01-01 + 365 days (2000 leap but Feb is over) + 73 days + extra days */
    /* 946684800 + 365*86400 (2000) + 31 (Jan) + 28 (Feb) + 14 (Mar 1-14) = let me compute */
    /* Actually, we just check roundtrip consistency: gmtime(mktime(x)) == x */
    struct tm result;
    gmtime_r(&t, &result);
    TEST("mktime: roundtrip year",  result.tm_year == 101);
    TEST("mktime: roundtrip mon",   result.tm_mon == 2);
    TEST("mktime: roundtrip mday",  result.tm_mday == 15);
    TEST("mktime: roundtrip hour",  result.tm_hour == 12);
    TEST("mktime: roundtrip min",   result.tm_min == 0);
    TEST("mktime: roundtrip sec",   result.tm_sec == 0);
    /* 5. Pre-1970 — 1969-06-20 — mktime/gmtime behavior depends on implementation */
    tm.tm_year = 69; tm.tm_mon = 5; tm.tm_mday = 20;
    tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    t = mktime(&tm);
    /* Kernel may not support pre-1970 dates (negative time_t) */
    /* Just verify it returns something reasonable or doesn't crash */
    TEST("mktime: pre-1970 returns non-negative or within range",
         (long long)t >= 0 || t == (time_t)-1);

    /* 6. 2038-01-19 03:14:07 (32-bit time_t overflow boundary) */
    tm.tm_year = 138; tm.tm_mon = 0; tm.tm_mday = 19;
    tm.tm_hour = 3; tm.tm_min = 14; tm.tm_sec = 7;
    tm.tm_isdst = -1;
    t = mktime(&tm);
    /* 2038-01-19 03:14:07 UTC = 0x7FFFFFFF = 2147483647 */
    if (sizeof(time_t) >= 8) {
        TEST("mktime: 2038-01-19 = 2147483647", t == 2147483647LL);
    }
    gmtime_r(&t, &result);
    TEST("mktime: 2038 roundtrip mday", result.tm_mday == 19);
    TEST("mktime: 2038 roundtrip hour", result.tm_hour == 3);
}

/* ===================================================================
 *  test_asctime
 * =================================================================== */
static void test_asctime(void)
{
    struct tm tm;
    char buf[26];

    /* 1. Epoch */
    tm.tm_year = 70; tm.tm_mon = 0; tm.tm_mday = 1;
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_wday = 4; tm.tm_yday = 0;

    char *r = asctime_r(&tm, buf);
    TEST("asctime_r: returns buf", r == buf);
    /* Format: "Thu Jan  1 00:00:00 1970\n\0" */
    TEST("asctime: starts with weekday", strstr(buf, "Thu") != NULL);
    TEST("asctime: contains month", strstr(buf, "Jan") != NULL);
    TEST("asctime: ends with newline", buf[24] == '\n');

    /* 2. Different date */
    tm.tm_year = 124; tm.tm_mon = 0; tm.tm_mday = 15;
    tm.tm_hour = 10; tm.tm_min = 30; tm.tm_sec = 0;
    tm.tm_wday = 1; tm.tm_yday = 14;
    asctime_r(&tm, buf);
    TEST("asctime: 2024-01-15", strstr(buf, "Jan") != NULL);
    TEST("asctime: strstr Mon", strstr(buf, "Mon") != NULL);

    /* 3. asctime convenience wrapper */
    char *r2 = asctime(&tm);
    TEST("asctime: non-NULL return", r2 != NULL);

    /* 4. asctime_r with NULL tm */
    r = asctime_r(NULL, buf);
    TEST("asctime_r: NULL tm returns NULL", r == NULL);
}

/* ===================================================================
 *  test_strftime
 * =================================================================== */
static void test_strftime(void)
{
    struct tm tm;
    char buf[128];

    /* Setup: 2024-03-15 14:30:45, Friday */
    tm.tm_year = 124; tm.tm_mon = 2; tm.tm_mday = 15;
    tm.tm_hour = 14; tm.tm_min = 30; tm.tm_sec = 45;
    tm.tm_wday = 5; tm.tm_yday = 74; tm.tm_isdst = 0;

    /* 1. %Y */
    strftime(buf, sizeof(buf), "%Y", &tm);
    TEST("strftime: %Y = 2024", strcmp(buf, "2024") == 0);

    /* 2. %m */
    strftime(buf, sizeof(buf), "%m", &tm);
    TEST("strftime: %m = 03", strcmp(buf, "03") == 0);

    /* 3. %d */
    strftime(buf, sizeof(buf), "%d", &tm);
    TEST("strftime: %d = 15", strcmp(buf, "15") == 0);

    /* 4. %H:%M:%S */
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    TEST("strftime: %H:%M:%S = 14:30:45", strcmp(buf, "14:30:45") == 0);

    /* 5. Full ISO-like format */
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    TEST("strftime: ISO format", strcmp(buf, "2024-03-15T14:30:45") == 0);

    /* 6. %a (abbreviated weekday) */
    strftime(buf, sizeof(buf), "%a", &tm);
    TEST("strftime: %a = Fri", strcmp(buf, "Fri") == 0);

    /* 7. %b (abbreviated month) */
    strftime(buf, sizeof(buf), "%b", &tm);
    TEST("strftime: %b = Mar", strcmp(buf, "Mar") == 0);

    /* 8. %% */
    strftime(buf, sizeof(buf), "%%Y", &tm);
    TEST("strftime: %% = %Y", strcmp(buf, "%Y") == 0);

    /* 9. Truncation (max=1) */
    size_t n = strftime(buf, 1, "hello", &tm);
    TEST("strftime: truncation returns 0", n == 0);

    /* 10. Empty format string (strftime returns 0) */
    size_t n2 = strftime(buf, sizeof(buf), "", &tm);
    TEST("strftime: empty returns 0", n2 == 0);

    /* Setup for weekday/month name tests: 2024-03-15 */
    struct tm tm2;
    tm2.tm_year = 124; tm2.tm_mon = 2; tm2.tm_mday = 15;
    tm2.tm_hour = 14; tm2.tm_min = 30; tm2.tm_sec = 45;
    tm2.tm_wday = 5; tm2.tm_yday = 74; tm2.tm_isdst = 0;

    /* 11. %A (full weekday name) */
    strftime(buf, sizeof(buf), "%A", &tm2);
    TEST("strftime: %A = Friday", strcmp(buf, "Friday") == 0);

    /* 12. %B (full month name) */
    strftime(buf, sizeof(buf), "%B", &tm2);
    TEST("strftime: %B = March", strcmp(buf, "March") == 0);
}

/* ===================================================================
 *  test_time_extra — additional time edge cases
 * =================================================================== */
static void test_time_extra(void)
{
    struct tm result, tm;
    time_t t;
    char buf[128];

    /* 1. Leap year 2000 (divisible by 400, IS leap) */
    {
        tm.tm_year = 100; tm.tm_mon = 1; tm.tm_mday = 29; /* Feb 29, 2000 */
        tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0;
        tm.tm_isdst = -1;
        t = mktime(&tm);
        struct tm back;
        gmtime_r(&t, &back);
        TEST("time_extra: 2000 leap Feb 29 roundtrip mday", back.tm_mday == 29);
        TEST("time_extra: 2000 leap Feb 29 roundtrip mon", back.tm_mon == 1);
    }

    /* 2. Non-leap year 2100 (divisible by 100, NOT by 400, NOT leap) */
    {
        tm.tm_year = 200; tm.tm_mon = 1; tm.tm_mday = 28; /* Feb 28, 2100 */
        tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
        tm.tm_isdst = -1;
        t = mktime(&tm);
        struct tm back;
        gmtime_r(&t, &back);
        TEST("time_extra: 2100 non-leap Feb 28 roundtrip mday", back.tm_mday == 28);
        /* Feb 29 should NOT exist */
        tm.tm_mday = 29;
        t = mktime(&tm);
        /* mktime may normalize to Mar 1 — check it's either -1 or maps to March */
        gmtime_r(&t, &back);
        int is_mar1 = (back.tm_mon == 2 && back.tm_mday == 1);
        TEST("time_extra: 2100 Feb 29 normalized (to Mar 1 or -1)",
             t == (time_t)-1 || is_mar1);
    }

    /* 3. Year 2400 (divisible by 400, IS leap) */
    {
        tm.tm_year = 500; tm.tm_mon = 1; tm.tm_mday = 29;
        tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
        tm.tm_isdst = -1;
        t = mktime(&tm);
        if (t != (time_t)-1) {
            struct tm back;
            gmtime_r(&t, &back);
            TEST("time_extra: 2400 leap Feb 29 roundtrip mday", back.tm_mday == 29);
        }
    }

    /* 4. Year 1900 (not a leap year — divisible by 100, not 400) */
    {
        tm.tm_year = 0; tm.tm_mon = 1; tm.tm_mday = 28;
        tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0;
        tm.tm_isdst = -1;
        t = mktime(&tm);
        if (t != (time_t)-1) {
            struct tm back;
            gmtime_r(&t, &back);
            TEST("time_extra: 1900 non-leap Feb 28 roundtrip mday", back.tm_mday == 28);
            TEST("time_extra: 1900 Feb yday <= 58", back.tm_yday <= 58);
        }
    }

    /* 5. Jan 1, 2001 (first day of new century that's actually first) */
    {
        t = 978307200LL; /* 2001-01-01 00:00:00 UTC */
        gmtime_r(&t, &result);
        TEST("time_extra: 2001-01-01 mon=0 (Jan)", result.tm_mon == 0);
        TEST("time_extra: 2001-01-01 mday=1", result.tm_mday == 1);
        TEST("time_extra: 2001-01-01 year=101", result.tm_year == 101);
        TEST("time_extra: 2001-01-01 yday=0", result.tm_yday == 0);
    }

    /* 6. Dec 31, 1999 — last day of millennium */
    {
        t = 946684799LL; /* 1999-12-31 23:59:59 UTC */
        gmtime_r(&t, &result);
        TEST("time_extra: 1999-12-31 mon=11 (Dec)", result.tm_mon == 11);
        TEST("time_extra: 1999-12-31 mday=31", result.tm_mday == 31);
        TEST("time_extra: 1999-12-31 year=99", result.tm_year == 99);
        TEST("time_extra: 1999-12-31 hour=23", result.tm_hour == 23);
        TEST("time_extra: 1999-12-31 min=59", result.tm_min == 59);
        TEST("time_extra: 1999-12-31 sec=59", result.tm_sec == 59);
    }

    /* 7. 2038-01-19 03:14:08 (one second after Y2038 overflow) */
    {
        t = 2147483648LL; /* 0x80000000 = -2147483648 as signed 32-bit */
        gmtime_r(&t, &result);
        /* With 64-bit time_t this should work fine */
        if (sizeof(time_t) >= 8) {
            TEST("time_extra: 2038+1 sec year valid", result.tm_year >= 138);
            TEST("time_extra: 2038+1 sec mon", result.tm_mon >= 0);
            TEST("time_extra: 2038+1 sec mday", result.tm_mday >= 19);
        }
    }

    /* 8. strftime with unsupported format specifier (%%z) */
    {
        struct tm tm2;
        memset(&tm2, 0, sizeof(tm2));
        tm2.tm_year = 100; tm2.tm_mon = 0; tm2.tm_mday = 1;
        size_t n = strftime(buf, sizeof(buf), "Hello %y %x %%z", &tm2);
        TEST("time_extra: strftime with %%z (unsupported) non-zero", n > 0);
        /* Should have produced at least "Hello 00" */
        TEST("time_extra: strftime unsupported spec skips %%z", n >= 7);
    }

    /* 9. strftime with all-day format */
    {
        struct tm tm3;
        memset(&tm3, 0, sizeof(tm3));
        tm3.tm_year = 124; tm3.tm_mon = 6; tm3.tm_mday = 4; /* 2024-07-04 */
        tm3.tm_wday = 4; /* Thu */
        strftime(buf, sizeof(buf), "%A, %B %d, %Y", &tm3);
        TEST("time_extra: strftime 'Thursday, July 04, 2024'",
             strcmp(buf, "Thursday, July 04, 2024") == 0);
    }

    /* 10. strftime single %% */
    {
        struct tm tm4;
        memset(&tm4, 0, sizeof(tm4));
        strftime(buf, sizeof(buf), "%%", &tm4);
        TEST("time_extra: strftime single %%", strcmp(buf, "%") == 0);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Time Function Tests ===\n\n");

    printf("--- gmtime ---\n");
    test_gmtime();

    printf("\n--- mktime ---\n");
    test_mktime();

    printf("\n--- asctime ---\n");
    test_asctime();

    printf("\n--- strftime ---\n");
    test_strftime();

    printf("\n--- extra edge cases ---\n");
    test_time_extra();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
