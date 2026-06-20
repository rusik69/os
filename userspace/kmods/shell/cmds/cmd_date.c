/* cmd_date.c — date command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_date(void) {
    struct rtc_time t;
    rtc_get_time(&t);
    kprintf("%lu-%02lu-%02lu %02lu:%02lu:%02lu\n",
            (unsigned long)t.year, (unsigned long)t.month, (unsigned long)t.day,
            (unsigned long)t.hour, (unsigned long)t.minute, (unsigned long)t.second);
}
