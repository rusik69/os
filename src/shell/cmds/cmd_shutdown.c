/* cmd_shutdown.c — shutdown command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_shutdown(void) {
    kprintf("Shutting down...\n");
    acpi_shutdown();
}
