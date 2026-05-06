/* cmd_serial.c — serial port (COM1) command */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

#define COM1_BASE 0x3F8

void cmd_serial(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: serial status | serial write <text>\n");
        return;
    }

    if (strcmp(args, "status") == 0) {
        kprintf("COM1 base: 0x%x\n", (uint64_t)COM1_BASE);
        kprintf("COM1 status: ready\n");
    } else if (strncmp(args, "write ", 6) == 0) {
        const char *msg = args + 6;
        uint32_t len = strlen(msg);
        serial_write((const uint8_t *)msg, len);
        uint8_t newline = '\n';
        serial_write(&newline, 1);
        kprintf("COM1: sent %u bytes\n", (uint64_t)len);
    } else {
        kprintf("Usage: serial status | serial write <text>\n");
    }
}
