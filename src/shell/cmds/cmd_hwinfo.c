/* cmd_hwinfo.c — comprehensive hardware summary */
#include "shell_cmds.h"
#include "libc.h"

void cmd_hwinfo(void) {
    libc_hwinfo_print();
}
