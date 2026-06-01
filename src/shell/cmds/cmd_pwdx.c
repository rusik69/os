#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_pwdx(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: pwdx <args>\n");
        return;
    }
    kprintf("pwdx: %s\n", args);
}
