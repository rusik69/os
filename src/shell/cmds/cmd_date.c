/* cmd_date.c — date command */
#include "shell_cmds.h"
#include "printf.h"
#include "rtc.h"

void cmd_date(void) {
    struct rtc_time t;
    rtc_get_time(&t);
    kprintf("%u-%02u-%02u %02u:%02u:%02u\n",
            (uint64_t)t.year, (uint64_t)t.month, (uint64_t)t.day,
            (uint64_t)t.hour, (uint64_t)t.minute, (uint64_t)t.second);
}
