#include "libc.h"
#include "printf.h"
#include "shell.h"
#include "shell_cmds.h"
#include "string.h"

void cmd_pwd(void) {
    char cwd[256];
    if (libc_getcwd(cwd, (int)sizeof(cwd)) == 0 && cwd[0])
        kprintf("%s\n", cwd);
    else
        kprintf("/\n");
}
