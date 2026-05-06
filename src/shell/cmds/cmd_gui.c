#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"

void cmd_gui(void) {
    int rc = libc_gui_shell_run();
    if (rc < 0) {
        kprintf("gui: failed to start desktop\n");
    }
}
