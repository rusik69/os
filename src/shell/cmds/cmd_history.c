#include "shell.h"
#include "shell_cmds.h"
void cmd_history_show(void) {
    extern void shell_history_show_entries(void);
    shell_history_show_entries();
}
