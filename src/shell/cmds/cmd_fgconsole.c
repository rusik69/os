/* cmd_fgconsole.c — print foreground console number (return 0) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_fgconsole(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* Always report console 1 as the foreground */
    kprintf("1\n");
    return 0;
}
