/* cmd_adjtimex.c — get/set clock adjustment */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

/* SYS_GETTIMEOFDAY gives us timeval */
#define SYS_GETTIMEOFDAY 200

int cmd_adjtimex(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Get current time of day via syscall */
    uint64_t tv_sec = 0, tv_usec = 0;
    struct { uint64_t sec; uint64_t usec; } tv;

    if (libc_syscall(SYS_GETTIMEOFDAY, (uint64_t)(uintptr_t)&tv, 0, 0, 0, 0) == 0) {
        tv_sec = tv.sec;
        tv_usec = tv.usec;
    }

    uint64_t ticks = libc_uptime_ticks();
    uint64_t total_sec = ticks / TIMER_FREQ;
    uint64_t days = total_sec / 86400;
    uint64_t hours = (total_sec % 86400) / 3600;
    uint64_t minutes = (total_sec % 3600) / 60;
    uint64_t seconds = total_sec % 60;

    kprintf("Clock adjustment:\n");
    kprintf("  System time:    %llu.%06llu sec since epoch\n",
            (unsigned long long)tv_sec, (unsigned long long)tv_usec);
    kprintf("  Uptime:         %llu day%s %02llu:%02llu:%02llu\n",
            (unsigned long long)days, days == 1 ? "" : "s",
            (unsigned long long)hours, (unsigned long long)minutes,
            (unsigned long long)seconds);
    kprintf("  Timer frequency: %d Hz\n", TIMER_FREQ);
    kprintf("  Ticks:          %llu\n", (unsigned long long)ticks);

    return 0;
}

void adjtimex_init(void)
{
    kprintf("[OK] cmd_adjtimex: clock adjustment command ready\n");
}
