/* cmd_uptime.c — uptime command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_uptime(void) {
    uint64_t ticks = libc_uptime_ticks();
    uint64_t seconds = ticks / TIMER_FREQ;
    uint64_t minutes = seconds / 60;
    seconds %= 60;
    kprintf("Uptime: %u min %u sec (%u ticks)\n", minutes, seconds, ticks);
}
