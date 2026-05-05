/* cmd_yes.c — Output a string repeatedly */

#include "shell_cmds.h"
#include "printf.h"

void cmd_yes(const char *args) {
    const char *str = (args && args[0]) ? args : "y";
    /* Print limited number of lines (no infinite loop in kernel) */
    for (int i = 0; i < 20; i++) {
        kprintf("%s\n", str);
    }
}
