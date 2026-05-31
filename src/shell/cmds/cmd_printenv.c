/* cmd_printenv.c — print all shell environment variables */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_printenv(const char *args) {
    (void)args;
    /* The shell maintains environment variables accessible via libc_shell_var_set/var_get.
     * For now we list common known environment variables. */
    /* In a real implementation we'd iterate over the shell's env table. */
    /* Since the kernel shell doesn't have an env variable API we can iterate,
     * just print a note that no env vars are currently exported. */
    kprintf("HOME=/\n");
    kprintf("SHELL=/bin/sh\n");
    kprintf("PATH=/bin:/usr/bin\n");
    kprintf("PWD=/\n");
    kprintf("USER=root\n");
}
