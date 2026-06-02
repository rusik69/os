/* cmd_renice.c — change nice value for an existing process */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "syscall.h"

static int parse_int_arg(const char **args, int *out) {
    const char *p = *args;
    while (*p == ' ') p++;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }
    if (!(*p >= '0' && *p <= '9')) return -1;
    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
    }
    *args = p;
    *out = value * sign;
    return 0;
}

static int parse_uint_arg(const char **args, uint32_t *out) {
    const char *p = *args;
    while (*p == ' ') p++;
    if (!(*p >= '0' && *p <= '9')) return -1;
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (uint32_t)(*p - '0');
        p++;
    }
    *args = p;
    *out = value;
    return 0;
}

void cmd_renice(const char *args) {
    int nice_val = 0;
    uint32_t pid = 0;
    if (!args || parse_int_arg(&args, &nice_val) != 0 || parse_uint_arg(&args, &pid) != 0) {
        kprintf("Usage: renice <nice_value> <pid>\n");
        kprintf("  nice_value: -20 (highest priority) to +19 (lowest priority)\n");
        return;
    }

    /* Clamp to valid range */
    if (nice_val < NICE_MIN) nice_val = NICE_MIN;
    if (nice_val > NICE_MAX) nice_val = NICE_MAX;

    if (libc_setpriority_pid(pid, nice_val) != 0) {
        kprintf("renice: no such process: %u\n", (unsigned long)pid);
        return;
    }
    kprintf("%u (nice %d)\n", (unsigned long)pid, nice_val);
}
