/* cmd_locale.c — get locale info */
#include "shell_cmds.h"
#include "printf.h"

void cmd_locale(const char *args) {
    (void)args;
    kprintf("LANG=POSIX\n");
    kprintf("LC_CTYPE=POSIX\n");
    kprintf("LC_NUMERIC=POSIX\n");
    kprintf("LC_TIME=POSIX\n");
    kprintf("LC_COLLATE=POSIX\n");
    kprintf("LC_MONETARY=POSIX\n");
    kprintf("LC_MESSAGES=POSIX\n");
    kprintf("LC_ALL=POSIX\n");
}
