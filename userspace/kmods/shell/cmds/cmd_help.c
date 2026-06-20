/* cmd_help.c — help command */
#include "shell_cmds.h"
#include "shell_cmd_table.h"
#include "printf.h"

void cmd_help(void) {
    kprintf("Available commands:\n");
    int n = shell_cmd_count();
    for (int i = 0; i < n; i++) {
        const shell_cmd_entry_t *e = shell_cmd_entry(i);
        kprintf("  %-12s - %s\n", e->name, e->desc);
    }
    kprintf("\nShell features: Tab completion, history (up/down),\n");
    kprintf("  pipes (cmd1 | cmd2), redirection (cmd > file, cmd >> file)\n");
    kprintf("  background execution (cmd &)\n");
    kprintf("  variables: NAME=value, $NAME expansion\n");
}
