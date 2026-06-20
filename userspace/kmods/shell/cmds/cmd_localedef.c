#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_localedef(const char *args) {
    if (!args) { kprintf("Usage: localedef -i <input> -f <charset> <locale>\n"); return; }
    kprintf("localedef: defining locale '%s'\n", args);
}
