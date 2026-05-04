/* cmd_seq.c — seq command */
#include "shell_cmds.h"
#include "printf.h"

static uint32_t parse_uint(const char **s) {
    uint32_t v = 0;
    while (**s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; }
    return v;
}

void cmd_seq(const char *args) {
    if (!args) { kprintf("Usage: seq <end> or seq <start> <end>\n"); return; }
    const char *p = args;
    uint32_t a = parse_uint(&p);
    while (*p == ' ') p++;
    uint32_t start = 1, end = a;
    if (*p >= '0' && *p <= '9') {
        start = a;
        end = parse_uint(&p);
    }
    if (end > 1000) end = 1000;
    for (uint32_t i = start; i <= end; i++)
        kprintf("%u\n", (uint64_t)i);
}
