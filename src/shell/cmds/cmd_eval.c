/* cmd_eval.c — eval builtin: execute arguments as shell command */
#include "shell_cmds.h"
#include "shell.h"
#include "printf.h"
#include "string.h"

void cmd_eval(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: eval <shell_command>\n");
        return;
    }

    /* Expand variables in the argument string */
    char expanded[512];
    int ret = shell_var_expand(args, expanded, sizeof(expanded));
    if (ret < 0) {
        kprintf("eval: expansion error\n");
        return;
    }

    if (expanded[0] == '\0') {
        return;
    }

    /* Execute the expanded string as a shell command */
    /* shell_exec_cmd expects (cmd, args) where cmd is the command and args are its arguments.
     * We need to split expanded into command + args. */
    const char *cmd_start = expanded;
    while (*cmd_start == ' ') cmd_start++;

    const char *cmd_end = cmd_start;
    while (*cmd_end && *cmd_end != ' ') cmd_end++;

    char cmd_name[64];
    int n = (int)(cmd_end - cmd_start);
    if (n >= 64) n = 63;
    memcpy(cmd_name, cmd_start, (size_t)n);
    cmd_name[n] = '\0';

    const char *cmd_args = cmd_end;
    while (*cmd_args == ' ') cmd_args++;

    shell_exec_cmd(cmd_name, cmd_args);
}
