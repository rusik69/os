#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"

void cmd_doom(const char *args) {
    (void)args;
    int rc = libc_doom_run();
    if (rc == -2) {
        kprintf("doom: already running\n");
    } else if (rc < 0) {
        kprintf("doom: failed to start (framebuffer required — use: make run)\n");
    }
}
