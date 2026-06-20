#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "vfs.h"
void cmd_isosize(const char *args) {
    if (!args) { kprintf("Usage: isosize <device>\n"); return; }
    struct vfs_stat st;
    if (vfs_stat(args, &st) == 0)
        kprintf("sector count: %lu\n", st.size / 2048UL);
    else
        kprintf("isosize: %s: cannot stat\n", args);
}
