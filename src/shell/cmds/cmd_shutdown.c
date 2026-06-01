#include "shell.h"
#include "shell_cmds.h"
void cmd_shutdown(void) {
    extern void acpi_shutdown(void);
    acpi_shutdown();
}
