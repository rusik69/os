#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_locale(const char *args) {
    (void)args;
    kprintf("LANG=POSIX\nLC_CTYPE=POSIX\n");
}
