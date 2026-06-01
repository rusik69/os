/* cmd_mesg.c — control write access to terminal */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_mesg(void) {
    kprintf("mesg: access control is not supported\n");
}
