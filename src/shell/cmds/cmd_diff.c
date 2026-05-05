/* cmd_diff.c — Compare two files line by line */

#include "shell_cmds.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"

void cmd_diff(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: diff <file1> <file2>\n");
        return;
    }

    /* Parse two filenames */
    char f1[64], f2[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) f1[i++] = *p++;
    f1[i] = '\0';
    while (*p == ' ') p++;
    if (!*p) { kprintf("diff: need two files\n"); return; }
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
        kprintf("diff: cannot read '%s'\n", f1);
        return;
    }
    if (vfs_read(path2, buf2, 2047, &sz2) != 0) {
        kprintf("diff: cannot read '%s'\n", f2);
        return;
    }
    buf1[sz1] = '\0';
    buf2[sz2] = '\0';

    /* Simple line-by-line comparison */
    char *l1 = buf1, *l2 = buf2;
    int lineno = 0;
    int diffs = 0;

    while (*l1 || *l2) {
        lineno++;
        char *e1 = l1, *e2 = l2;
        while (*e1 && *e1 != '\n') e1++;
        while (*e2 && *e2 != '\n') e2++;

        int len1 = e1 - l1;
        int len2 = e2 - l2;

        int differ = (len1 != len2) || memcmp(l1, l2, len1) != 0;
        if (differ) {
            diffs++;
            kprintf("%dc%d\n", (uint64_t)lineno, (uint64_t)lineno);
            kprintf("< ");
            for (int j = 0; j < len1; j++) kprintf("%c", (uint64_t)(uint8_t)l1[j]);
            kprintf("\n---\n> ");
            for (int j = 0; j < len2; j++) kprintf("%c", (uint64_t)(uint8_t)l2[j]);
            kprintf("\n");
        }

        l1 = (*e1 == '\n') ? e1 + 1 : e1;
        l2 = (*e2 == '\n') ? e2 + 1 : e2;
    }

    if (diffs == 0) kprintf("Files are identical\n");
}
