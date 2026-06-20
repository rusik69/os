/* cmd_comm.c -- Compare two sorted files line by line */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

/* Read a file into a static buffer. Returns number of lines or -1 on error. */
static int read_file_lines(const char *path, char *buf, int bufsz,
                           char *lines[], int max_lines)
{
    uint32_t size = 0;
    char fullpath[64];
    if (path[0] != '/') {
        fullpath[0] = '/';
        strncpy(fullpath + 1, path, 61);
        fullpath[62] = '\0';
    } else {
        strncpy(fullpath, path, 63);
        fullpath[63] = '\0';
    }
    /* Trim trailing spaces */
    int pl = (int)strlen(fullpath);
    while (pl > 0 && fullpath[pl - 1] == ' ') fullpath[--pl] = '\0';

    if (vfs_read(fullpath, buf, (uint32_t)(bufsz - 1), &size) != 0) {
        kprintf("comm: cannot read '%s'\n", path);
        return -1;
    }
    buf[size] = '\0';

    int n = 0;
    char *p = buf;
    while (*p && n < max_lines) {
        lines[n++] = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = '\0'; p++; }
    }
    return n;
}

int cmd_comm(int argc, char **argv) {
    int flag1 = 1, flag2 = 1, flag3 = 1;
    int i = 1;

    while (i < argc && argv[i][0] == '-') {
        char *opt = argv[i] + 1;
        while (*opt) {
            if (*opt == '1')      flag1 = 0;
            else if (*opt == '2') flag2 = 0;
            else if (*opt == '3') flag3 = 0;
            else {
                kprintf("comm: invalid option -- '%c'\n", *opt);
                return 1;
            }
            opt++;
        }
        i++;
    }

    if (argc - i < 2) {
        kprintf("Usage: comm [-123] <file1> <file2>\n");
        return 1;
    }

    static char buf1[8192], buf2[8192];
    char *lines1[1024], *lines2[1024];

    int n1 = read_file_lines(argv[i], buf1, sizeof(buf1), lines1, 1024);
    if (n1 < 0) return 1;
    int n2 = read_file_lines(argv[i + 1], buf2, sizeof(buf2), lines2, 1024);
    if (n2 < 0) return 1;

    int idx1 = 0, idx2 = 0;
    while (idx1 < n1 && idx2 < n2) {
        int cmp = strcmp(lines1[idx1], lines2[idx2]);
        if (cmp < 0) {
            if (flag1) kprintf("%s\n", lines1[idx1]);
            idx1++;
        } else if (cmp > 0) {
            if (flag2) kprintf("\t%s\n", lines2[idx2]);
            idx2++;
        } else {
            if (flag3) kprintf("\t\t%s\n", lines1[idx1]);
            idx1++;
            idx2++;
        }
    }
    while (idx1 < n1) {
        if (flag1) kprintf("%s\n", lines1[idx1]);
        idx1++;
    }
    while (idx2 < n2) {
        if (flag2) kprintf("\t%s\n", lines2[idx2]);
        idx2++;
    }
    return 0;
}
