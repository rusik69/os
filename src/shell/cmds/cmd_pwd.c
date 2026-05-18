/* cmd_pwd.c — print working directory */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"

void cmd_pwd(void) {
    char buf[64];
    if (libc_getcwd(buf, sizeof(buf)) == 0)
        kprintf("%s\n", buf);
    else
        kprintf("/\n");
}
