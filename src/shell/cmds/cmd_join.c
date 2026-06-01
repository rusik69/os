/* cmd_join.c -- Join lines of two files on a common field */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

/* Read a file into a static buffer, return array of lines, count. */
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
    int pl = (int)strlen(fullpath);
    while (pl > 0 && fullpath[pl - 1] == ' ') fullpath[--pl] = '\0';

    if (vfs_read(fullpath, buf, (uint32_t)(bufsz - 1), &size) != 0) {
        kprintf("join: cannot read '%s'\n", path);
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

/* Return pointer to the join field (first whitespace-delimited token). */
static char *get_field(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    return line;
}

/* Return length of the first field (up to whitespace or end). */
static int field_len(char *field) {
    int len = 0;
    while (field[len] && field[len] != ' ' && field[len] != '\t')
        len++;
    return len;
}

int cmd_join(int argc, char **argv) {
    if (argc < 3) {
        kprintf("Usage: join <file1> <file2>\n");
        return 1;
    }

    static char buf1[8192], buf2[8192];
    char *lines1[1024], *lines2[1024];

    int n1 = read_file_lines(argv[1], buf1, sizeof(buf1), lines1, 1024);
    if (n1 < 0) return 1;
    int n2 = read_file_lines(argv[2], buf2, sizeof(buf2), lines2, 1024);
    if (n2 < 0) return 1;

    int idx2 = 0;
    for (int idx1 = 0; idx1 < n1; idx1++) {
        char *f1 = get_field(lines1[idx1]);
        int fl1 = field_len(f1);

        /* Scan file2 for matching field */
        int matched = 0;
        for (int j = idx2; j < n2; j++) {
            char *f2 = get_field(lines2[j]);
            int fl2 = field_len(f2);

            if (fl1 == fl2 && strncmp(f1, f2, (size_t)fl1) == 0) {
                /* Output joined line: field + rest of file1 + rest of file2 */
                kprintf("%.*s", fl1, f1);
                char *rest1 = f1 + fl1;
                while (*rest1 == ' ' || *rest1 == '\t') rest1++;
                char *rest2 = f2 + fl2;
                while (*rest2 == ' ' || *rest2 == '\t') rest2++;
                if (*rest1) kprintf(" %s", rest1);
                if (*rest2) kprintf(" %s", rest2);
                kprintf("\n");
                matched = 1;
                idx2 = j + 1;
                break;
            }
        }
        /* If no match and we passed the last possible position, reset */
        if (!matched) {
            /* Just unpairable line from file1 - normally join suppresses these */
        }
    }
    return 0;
}
