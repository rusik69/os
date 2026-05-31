/* cmd_mesg.c — Control write access to terminal (stub) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

/* Global state: terminal write allowed (default yes) */
static int mesg_state = 1;  /* 1 = allowed, 0 = denied */

void cmd_mesg(const char *args) {
    if (!args || !args[0]) {
        /* Print current state */
        kprintf("mesg: terminal write access is %s\n",
                mesg_state ? "enabled (y)" : "disabled (n)");
        return;
    }

    if (args[0] == 'y' || args[0] == 'Y') {
        mesg_state = 1;
        kprintf("mesg: write access enabled\n");
    } else if (args[0] == 'n' || args[0] == 'N') {
        mesg_state = 0;
        kprintf("mesg: write access disabled\n");
    } else {
        kprintf("Usage: mesg [y|n]\n");
    }
}
