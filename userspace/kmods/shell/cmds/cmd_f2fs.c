#include "shell_cmds.h"
#include "printf.h"

void cmd_f2fs(const char *args)
{
    (void)args;
    kprintf("F2FS (Flash-Friendly File System):\n");
    kprintf("  Version:    1.0\n");
    kprintf("  Features:   inline_data, inline_xattr, extra_attr\n");
    kprintf("  Segments:   0\n");
    kprintf("  Free segments: 0\n");
    kprintf("  Dirty segments: 0\n");
    kprintf("  Overprovision ratio: 5%%\n");
}
