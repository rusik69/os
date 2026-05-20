/* shell_cmd_table.c — shared command name/description table */

#include "shell_cmd_table.h"
#include "string.h"

#include "cmd_table.inc"

int shell_cmd_count(void) {
    int n = 0;
    while (shell_cmd_table[n].name) n++;
    return n;
}

const shell_cmd_entry_t *shell_cmd_entry(int idx) {
    if (idx < 0 || idx >= shell_cmd_count()) return 0;
    return &shell_cmd_table[idx];
}

const char *shell_cmd_lookup_desc(const char *name) {
    for (int i = 0; shell_cmd_table[i].name; i++) {
        if (strcmp(shell_cmd_table[i].name, name) == 0)
            return shell_cmd_table[i].desc;
    }
    return 0;
}
