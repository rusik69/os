/* cmd_sleep.c — sleep command */
#include "shell_cmds.h"
#include "printf.h"
#include "timer.h"
#include "scheduler.h"

static uint32_t parse_uint(const char **s) {
    uint32_t v = 0;
    while (**s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; }
    return v;
}

void cmd_sleep(const char *args) {
    if (!args || !(*args >= '0' && *args <= '9')) {
        kprintf("Usage: sleep <seconds>\n");
        return;
    }
    const char *p = args;
    uint32_t sec = parse_uint(&p);
    if (sec > 60) sec = 60;
    uint64_t target = timer_get_ticks() + (uint64_t)sec * TIMER_FREQ;
    while (timer_get_ticks() < target)
        scheduler_yield();
    kprintf("Slept %u seconds\n", (uint64_t)sec);
}
