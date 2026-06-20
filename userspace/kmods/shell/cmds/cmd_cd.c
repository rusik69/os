/* cmd_cd.c — change current working directory */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_cd(const char *args) {
    const char *path = (args && args[0]) ? args : "/";
    if (libc_chdir(path) < 0)
        kprintf("cd: %s: no such directory\n", path);
}
