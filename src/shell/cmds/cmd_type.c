/* cmd_type.c — type builtin: describe how a command would be interpreted */
#include "shell_cmds.h"
#include "shell.h"
#include "printf.h"
#include "string.h"
#include "shell_cmd_table.h"

void cmd_type(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: type <name> [<name>...]\n");
        return;
    }

    const char *p = args;
    while (*p) {
        char name[64];
        int ni = 0;
        while (*p && *p != ' ' && ni < 63) name[ni++] = *p++;
        name[ni] = '\0';
        while (*p == ' ') p++;

        if (ni == 0) continue;

        int found = 0;

        /* Check if it's a shell builtin */
        int ncmds = shell_cmd_count();
        for (int i = 0; i < ncmds; i++) {
            if (strcmp(shell_cmd_names[i], name) == 0) {
                kprintf("%s is a shell builtin\n", name);
                found = 1;
                break;
            }
        }

        if (!found) {
            const char *val = shell_var_get(name);
            if (val && *val) {
                if (val[0] == '(' && val[1] == ')') {
                    kprintf("%s is a function\n", name);
                } else {
                    kprintf("%s is aliased to '%s'\n", name, val);
                }
                found = 1;
            }
        }

        if (!found) {
            kprintf("%s: not found\n", name);
        }
    }
}
