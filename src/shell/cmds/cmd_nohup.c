/* cmd_nohup.c — run command immune to hangups (just exec the args) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_nohup(const char *args) {
    if (!args || !*args) { kprintf("Usage: nohup <command> [args...]\n"); return; }

    /* In a real system nohup would redirect SIGHUP and redirect output.
     * In this simple kernel shell, we just echo and attempt to run the command. */
    kprintf("nohup: ignoring SIGHUP, running: %s\n", args);

    /* Try to execute the first argument as a path */
    char cmd[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) cmd[i++] = *p++;
    cmd[i] = '\0';

    char fpath[64];
    if (cmd[0] != '/') { fpath[0] = '/'; strncpy(fpath+1, cmd, 62); fpath[63] = '\0'; }
    else strncpy(fpath, cmd, 63);
    fpath[63] = '\0';

    if (elf_exec(fpath) == 0) return;

    /* Fallback: execute via shell */
    libc_shell_exec_cmd(cmd, p);
}
