#include "shell.h"
#include "shell_cmds.h"
#include "string.h"

/* history_persist API */
extern void history_persist_save(void);
extern void history_persist_load(void);

void cmd_history_show(void) {
    extern void shell_history_show_entries(void);
    shell_history_show_entries();
}

/* history [-w] [-r] — show, write, or read history */
void cmd_history(int argc, char **argv) {
    if (argc < 2) {
        cmd_history_show();
        return;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) {
            history_persist_save();
        } else if (strcmp(argv[i], "-r") == 0) {
            history_persist_load();
        } else {
            cmd_history_show();
        }
    }
}
