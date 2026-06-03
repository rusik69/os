#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

/* dos_exec is declared in elf.h or we declare it here */
int dos_exec(const char *path);

void cmd_dosbox(const char *args) {
    if (!args || !*args) {
        kprintf("usage: dosbox <path>\n");
        return;
    }
    /* Skip leading spaces */
    while (*args == ' ') args++;
    if (!*args) {
        kprintf("usage: dosbox <path>\n");
        return;
    }
    kprintf("dosbox: loading %s...\n", args);
    int ret = dos_exec(args);
    if (ret < 0) {
        kprintf("dosbox: failed with error %d\n", (int)ret);
    } else {
        kprintf("dosbox: program exited\n");
    }
}
