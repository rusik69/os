#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "heap.h"

void cmd_dmesg(const char *args) {
    /* Skip leading spaces */
    while (args && *args == ' ') args++;
    int do_clear = (args && args[0] == '-' && args[1] == 'c');

    /* The ring buffer is 64 KB — allocate on heap to avoid stack pressure
     * and read the full content regardless of size. */
    char *buf = (char *)kmalloc(65536);
    if (!buf) {
        kprintf("dmesg: out of memory\n");
        return;
    }

    int n = kprintf_dmesg(buf, 65536);
    kprintf("%s", buf);
    kfree(buf);

    if (do_clear)
        kprintf_dmesg_clear();
}
