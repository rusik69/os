/* cmd_hexdump.c — hexdump command */
#include "shell_cmds.h"
#include "printf.h"

void cmd_hexdump(const char *args) {
    if (!args) { kprintf("Usage: hexdump <addr> [len]\n"); return; }
    uint64_t addr = 0;
    if (args[0] == '0' && args[1] == 'x') args += 2;
    while (1) {
        char c = *args;
        if (c >= '0' && c <= '9') addr = addr * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') addr = addr * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') addr = addr * 16 + (c - 'A' + 10);
        else break;
        args++;
    }
    while (*args == ' ') args++;
    uint64_t len = 64;
    if (*args >= '0' && *args <= '9') {
        len = 0;
        while (*args >= '0' && *args <= '9') { len = len * 10 + (*args - '0'); args++; }
    }
    if (len > 256) len = 256;
    uint8_t *ptr = (uint8_t *)addr;
    for (uint64_t i = 0; i < len; i += 16) {
        kprintf("%p: ", (uint64_t)(ptr + i));
        for (int j = 0; j < 16 && i + j < len; j++) kprintf("%x ", (uint64_t)ptr[i + j]);
        kprintf("\n");
    }
}
