/* cmd_who.c — show who is logged in */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

void cmd_who(const char *args) {
    (void)args;
    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read("/proc/users", buf, sizeof(buf)-1, &size) != 0) {
        kprintf("who: cannot list users\n");
        return;
    }
    buf[size] = '\0';
    kprintf("%s", buf);
}
