/* cmd_paste.c — Merge lines of files side by side */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_paste(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: paste <file1> <file2>\n");
        return;
    }

    /* Parse two filenames */
    char f1[64], f2[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) f1[i++] = *p++;
    f1[i] = '\0';
    while (*p == ' ') p++;
    if (!*p) { kprintf("paste: need two files\n"); return; }
    i = 0;
    while (*p && *p != ' ' && i < 63) f2[i++] = *p++;
    f2[i] = '\0';

    char path1[64], path2[64];
    if (f1[0] != '/') { path1[0] = '/'; strcpy(path1 + 1, f1); }
    else strcpy(path1, f1);
    if (f2[0] != '/') { path2[0] = '/'; strcpy(path2 + 1, f2); }
    else strcpy(path2, f2);

    static char buf1[2048], buf2[2048];
    uint32_t sz1 = 0, sz2 = 0;
    if (vfs_read(path1, buf1, 2047, &sz1) != 0) {
        kprintf("paste: cannot read '%s'\n", f1);
        return;
    }
    if (vfs_read(path2, buf2, 2047, &sz2) != 0) {
        kprintf("paste: cannot read '%s'\n", f2);
        return;
    }
    buf1[sz1] = '\0';
    buf2[sz2] = '\0';

    /* Merge line by line with tab separator */
    char *l1 = buf1, *l2 = buf2;
    while (*l1 || *l2) {
        /* Print line from file1 */
        if (*l1) {
            while (*l1 && *l1 != '\n') { kprintf("%c", (uint64_t)(uint8_t)*l1); l1++; }
            if (*l1 == '\n') l1++;
        }
        kprintf("\t");
        /* Print line from file2 */
        if (*l2) {
            while (*l2 && *l2 != '\n') { kprintf("%c", (uint64_t)(uint8_t)*l2); l2++; }
            if (*l2 == '\n') l2++;
        }
        kprintf("\n");
    }
}
