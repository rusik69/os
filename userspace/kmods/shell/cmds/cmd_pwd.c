#include "libc.h"
#include "printf.h"
#include "shell.h"
#include "shell_cmds.h"
#include "string.h"
#include "unistd.h"

void cmd_pwd(void) {
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) == 0)
        kprintf("%s\n", cwd);
    else
        kprintf("/\n");
}
