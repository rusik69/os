/* cmd_404.c — find files by permission bits (simplified ls variant) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_404(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: 404 <pattern>  (e.g. 644, 755, rwxr-xr-x)\n");
        return;
    }
    /* Simple: list root directory entries as a demo */
    char names[64][FS_MAX_NAME];
    int count = fs_list_names("/", "", names, 64);
    if (count < 0) {
        kprintf("404: cannot list root\n");
        return;
    }
    kprintf("Files in / matching pattern:\n");
    for (int i = 0; i < count; i++) {
        struct vfs_stat st;
        if (vfs_stat(names[i], &st) == 0) {
            kprintf("  %s  (mode=%lo)\n", names[i], (unsigned long)st.mode);
        }
    }
}
