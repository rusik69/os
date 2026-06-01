/* cmd_nproc.c — print number of processors */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_nproc(void) {
    kprintf("1\n");
}
