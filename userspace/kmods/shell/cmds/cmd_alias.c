/* cmd_alias.c — alias / unalias built-in commands */
#include "shell_cmds.h"
#include "shell.h"
#include "printf.h"
#include "string.h"

void cmd_alias(const char *args) {
    if (!args || !args[0]) {
        shell_alias_list();
        return;
    }
    /* Expect: name=value */
    const char *eq = args;
    while (*eq && *eq != '=') eq++;
    if (*eq != '=') {
        /* Just lookup */
        const char *v = shell_alias_get(args);
        if (v) kprintf("alias %s='%s'\n", args, v);
        else   kprintf("alias: %s not found\n", args);
        return;
    }
    char name[32]; int nl = (int)(eq - args);
    if (nl > 31) nl = 31;
    memcpy(name, args, nl); name[nl] = '\0';
    shell_alias_set(name, eq + 1);
}

void cmd_unalias(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: unalias <name>\n");
        return;
    }
    shell_alias_del(args);
}
