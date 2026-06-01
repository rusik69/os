#include "shell.h"
#include "shell_cmds.h"
void cmd_reboot(void) {
    extern void acpi_reboot(void);
    acpi_reboot();
}
