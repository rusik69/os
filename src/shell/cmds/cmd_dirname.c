/* cmd_dirname.c — strip path component */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

void cmd_dirname(const char *args)
{
    if (!args || !*args) {
        kprintf("dirname: missing operand\n");
        shell_set_exit_status(1);
        return;
    }

    const char *path = args;
    const char *last_slash = NULL;
    const char *p = path;

    while (*p) {
        if (*p == '/')
            last_slash = p;
        p++;
    }

    if (!last_slash) {
        kprintf(".\n");
    } else if (last_slash == path) {
        kprintf("/\n");
    } else {
        size_t len = (size_t)(last_slash - path);
        for (size_t i = 0; i < len; i++)
            kprintf("%c", path[i]);
        kprintf("\n");
    }

    shell_set_exit_status(0);
}

void dirname_init(void)
{
    kprintf("[OK] cmd_dirname: path component stripper ready\n");
}
