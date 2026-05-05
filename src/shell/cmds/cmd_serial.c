/* cmd_serial.c — serial port (COM1) command */
#include "shell_cmds.h"
#include "serial.h"
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
        kprintf("COM1 data: %s\n", serial_readable() ? "data available" : "no data");
    } else if (strncmp(args, "write ", 6) == 0) {
        const char *msg = args + 6;
        uint32_t len = strlen(msg);
        serial_write(msg);
        serial_putchar('\n');
        kprintf("COM1: sent %u bytes\n", (uint64_t)len);
    } else {
        kprintf("Usage: serial status | serial write <text>\n");
    }
}
