/* cmd_logger.c — Logger: print message with timestamp prefix */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_logger(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: logger <message>\n");
        return;
    }

    /* Get timestamp from timer ticks via libc */
    uint64_t ticks = libc_uptime_ticks();
    uint64_t secs = ticks / 100;  /* TIMER_FREQ is 100 */
    uint64_t msec = (ticks % 100) * 10;

    kprintf("[%llu.%03llu] %s\n",
            (unsigned long long)secs,
            (unsigned long long)msec,
            args);
}
