/* cmd_xargs.c -- Build and execute commands from piped input */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

/* xargs reads piped stdin (or a pipe file arg) and executes command with
 * the input as additional arguments. */
void cmd_xargs(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: cmd | xargs <command>\n");
        return;
    }

    extern void shell_exec_cmd(const char *cmd, const char *cmd_args);

    /* Parse: command_name [extra_args...] [pipe_file]
     * If there is a '/'-prefixed last argument, it's the pipe file (legacy).
     * Otherwise try stdin pipe. */
    char cmd_name[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) cmd_name[i++] = *p++;
    cmd_name[i] = '\0';
    while (*p == ' ') p++;

    static char buf[4096];
    uint32_t size = 0;

    if (shell_has_stdin()) {
        /* Read from piped stdin */
        size = (uint32_t)shell_stdin_read(buf, (int)sizeof(buf) - 1);
    } else {
        /* Legacy: look for pipe file as last /... arg */
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
        if (vfs_read(pipe_file, buf, 4095, &size) != 0) {
            kprintf("xargs: cannot read pipe input\n");
            return;
        }
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
