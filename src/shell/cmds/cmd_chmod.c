#include "shell_cmds.h"
#include "fs.h"
#include "string.h"
#include "printf.h"

/* chmod <mode> <path>
 * mode is octal: 755, 644, etc.
 */
void cmd_chmod(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: chmod <mode> <path>\n");
        kprintf("  mode is octal, e.g. 755 or 644\n");
        return;
    }

    /* Parse octal mode */
    const char *p = args;
    while (*p == ' ') p++;
    if (!*p) { kprintf("Usage: chmod <mode> <path>\n"); return; }

    uint16_t mode = 0;
    while (*p >= '0' && *p <= '7') {
        mode = (uint16_t)(mode * 8 + (*p - '0'));
        p++;
    }
    while (*p == ' ') p++;
    if (!*p) { kprintf("chmod: missing path\n"); return; }

    /* path */
    char path[64];
    if (*p != '/') { path[0] = '/'; strcpy(path + 1, p); }
    else strcpy(path, p);

    int rc = fs_chmod(path, mode);
    if (rc == -1) kprintf("chmod: not found: %s\n", path);
    else if (rc == -2) kprintf("chmod: permission denied\n");
    else {
        char mstr[10];
        fs_mode_str(mode, mstr);
        kprintf("mode changed: %s -> %s\n", path, mstr);
    }
}
