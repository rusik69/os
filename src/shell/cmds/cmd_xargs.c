/* cmd_xargs.c — Build and execute commands from piped input */

#include "shell_cmds.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"

/* xargs reads piped input file and executes command with input as args.
 * When used in a pipe: args = "<command> [extra_args] /.pipe_tmp" */
void cmd_xargs(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: cmd | xargs <command>\n");
        return;
    }

    extern void shell_exec_cmd(const char *cmd, const char *cmd_args);

    /* Parse: command_name [cmd_args...] pipe_file
     * The last argument is the pipe input file. */
    char cmd_name[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) cmd_name[i++] = *p++;
    cmd_name[i] = '\0';
    while (*p == ' ') p++;

    /* Find the pipe file (last arg starting with /) */
    const char *pipe_file = NULL;
    const char *last_space = NULL;
    for (const char *s = p; *s; s++) {
        if (*s == ' ') last_space = s;
    }
    if (last_space && last_space[1] == '/') {
        pipe_file = last_space + 1;
    } else if (p[0] == '/') {
        pipe_file = p;
    }

    if (!pipe_file) {
        kprintf("xargs: no pipe input\n");
        return;
    }

    /* Read pipe input */
    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(pipe_file, buf, 4095, &size) != 0) {
        kprintf("xargs: cannot read pipe input\n");
        return;
    }
    buf[size] = '\0';

    /* Replace newlines with spaces */
    for (uint32_t j = 0; j < size; j++) {
        if (buf[j] == '\n' || buf[j] == '\r') buf[j] = ' ';
    }
    /* Trim trailing spaces */
    while (size > 0 && buf[size-1] == ' ') buf[--size] = '\0';

    /* Execute command with combined args */
    shell_exec_cmd(cmd_name, size > 0 ? buf : NULL);
}
