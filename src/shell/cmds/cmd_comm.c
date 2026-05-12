/* cmd_comm.c — compare two sorted files line by line */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

/* comm: compare sorted files A and B, printing:
 *   col1: lines only in A
 *   col2: lines only in B
 *   col3: lines in both
 * Options: -1 suppress col1, -2 suppress col2, -3 suppress col3
 */
void cmd_comm(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: comm [-123] <file1> <file2>\n");
        return;
    }

    int sup1 = 0, sup2 = 0, sup3 = 0;
    const char *p = args;

    while (*p == '-') {
        p++;
        while (*p && *p != ' ') {
            if (*p == '1') sup1 = 1;
            else if (*p == '2') sup2 = 1;
            else if (*p == '3') sup3 = 1;
            p++;
        }
        while (*p == ' ') p++;
    }

    /* Parse two filenames */
    char path1[64], path2[64];
    int i = 0;
    while (*p && *p != ' ' && i < 63) path1[i++] = *p++;
    path1[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i < 63) path2[i++] = *p++;
    path2[i] = '\0';

    if (!path1[0] || !path2[0]) { kprintf("comm: need two files\n"); return; }

    /* Prepend / if needed */
    char f1[64], f2[64];
    if (path1[0] != '/') { f1[0] = '/'; strncpy(f1+1, path1, 62); }
    else strncpy(f1, path1, 63);
    f1[63] = '\0';
    if (path2[0] != '/') { f2[0] = '/'; strncpy(f2+1, path2, 62); }
    else strncpy(f2, path2, 63);
    f2[63] = '\0';

    static char buf1[4096], buf2[4096];
    uint32_t s1 = 0, s2 = 0;
    if (vfs_read(f1, buf1, sizeof(buf1)-1, &s1) != 0) {
        kprintf("comm: cannot read '%s'\n", f1); return;
    }
    if (vfs_read(f2, buf2, sizeof(buf2)-1, &s2) != 0) {
        kprintf("comm: cannot read '%s'\n", f2); return;
    }
    buf1[s1] = '\0'; buf2[s2] = '\0';

    /* Split into line arrays */
    char *lines1[256], *lines2[256];
    int n1 = 0, n2 = 0;
    char *q = buf1;
    while (*q && n1 < 256) {
        lines1[n1++] = q;
        while (*q && *q != '\n') q++;
        if (*q == '\n') { *q++ = '\0'; }
    }
    q = buf2;
    while (*q && n2 < 256) {
        lines2[n2++] = q;
        while (*q && *q != '\n') q++;
        if (*q == '\n') { *q++ = '\0'; }
    }

    /* Merge compare */
    int i1 = 0, i2 = 0;
    while (i1 < n1 || i2 < n2) {
        int cmp;
        if (i1 >= n1) cmp = 1;
        else if (i2 >= n2) cmp = -1;
        else cmp = strcmp(lines1[i1], lines2[i2]);

        if (cmp < 0) {
            if (!sup1) kprintf("%s\n", lines1[i1]);
            i1++;
        } else if (cmp > 0) {
            if (!sup2) kprintf("\t%s\n", lines2[i2]);
            i2++;
        } else {
            if (!sup3) kprintf("\t\t%s\n", lines1[i1]);
            i1++; i2++;
        }
    }
}
