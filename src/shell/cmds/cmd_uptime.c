/* cmd_uptime.c — uptime command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

void cmd_uptime(void) {
    uint64_t ticks = libc_uptime_ticks();
    uint64_t total_sec = ticks / TIMER_FREQ;
    uint64_t days = total_sec / 86400;
    uint64_t hours = (total_sec % 86400) / 3600;
    uint64_t minutes = (total_sec % 3600) / 60;
    uint64_t seconds = total_sec % 60;

    /* Default: standard format */
    if (days > 0)
        kprintf("Uptime: %llu day%s, %llu:%02llu:%02llu (%llu ticks)\n",
                (unsigned long long)days, days == 1 ? "" : "s",
                (unsigned long long)hours, (unsigned long long)minutes,
                (unsigned long long)seconds, (unsigned long long)ticks);
    else
        kprintf("Uptime: %llu:%02llu:%02llu (%llu ticks)\n",
                (unsigned long long)hours, (unsigned long long)minutes,
                (unsigned long long)seconds, (unsigned long long)ticks);
}
