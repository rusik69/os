/* cmd_join.c — join lines of two files on a common field */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

#define JOIN_LINES 512
#define JOIN_LINE_LEN 256
#define JOIN_BUF 8192

static int split_line(char *line, char fields[][JOIN_LINE_LEN], int maxf) {
    int n = 0;
    char *p = line;
    while (*p && n < maxf) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int fi = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && fi < JOIN_LINE_LEN-1)
            fields[n][fi++] = *p++;
        fields[n][fi] = '\0';
        n++;
    }
    return n;
}

static int read_lines(const char *path, char lines[][JOIN_LINE_LEN]) {
    static char buf[JOIN_BUF];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf)-1, &size) != 0) return -1;
    buf[size] = '\0';
    int n = 0;
    char *p = buf;
    while (*p && n < JOIN_LINES) {
        int li = 0;
        while (*p && *p != '\n' && li < JOIN_LINE_LEN-1)
            lines[n][li++] = *p++;
        lines[n][li] = '\0';
        n++;
        if (*p == '\n') p++;
    }
    return n;
}

void cmd_join(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: join [-j <field>] <file1> <file2>\n");
        return;
    }
    int join_field = 1;
    const char *p = args;
    char f1[64], f2[64];
    f1[0] = f2[0] = '\0';

    while (*p == '-') {
        p++;
        if (*p == 'j') {
            p++;
            if (*p >= '1' && *p <= '9') { join_field = *p - '0'; p++; }
        }
        while (*p == ' ') p++;
    }
    int i = 0;
    while (*p && *p != ' ' && i < 63) f1[i++] = *p++;
    f1[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i < 63) f2[i++] = *p++;
    f2[i] = '\0';
    if (!f1[0] || !f2[0]) { kprintf("join: need two files\n"); return; }

    char r1[64], r2[64];
    if (f1[0] != '/') { r1[0] = '/'; strncpy(r1+1, f1, 62); }
    else strncpy(r1, f1, 63);
    r1[63] = '\0';
    if (f2[0] != '/') { r2[0] = '/'; strncpy(r2+1, f2, 62); }
    else strncpy(r2, f2, 63);
    r2[63] = '\0';

    static char lines1[JOIN_LINES][JOIN_LINE_LEN];
    static char lines2[JOIN_LINES][JOIN_LINE_LEN];
    int n1 = read_lines(r1, lines1);
    int n2 = read_lines(r2, lines2);
    if (n1 < 0) { kprintf("join: cannot read '%s'\n", f1); return; }
    if (n2 < 0) { kprintf("join: cannot read '%s'\n", f2); return; }

    char fields1[16][JOIN_LINE_LEN], fields2[16][JOIN_LINE_LEN];
    for (int i = 0; i < n1; i++) {
        int nf1 = split_line(lines1[i], fields1, 16);
        if (join_field > nf1) continue;
        for (int j = 0; j < n2; j++) {
            int nf2 = split_line(lines2[j], fields2, 16);
            if (join_field > nf2) continue;
            if (strcmp(fields1[join_field-1], fields2[join_field-1]) == 0) {
                kprintf("%s ", fields1[join_field-1]);
                for (int fi = 0; fi < nf1; fi++)
                    if (fi != join_field-1) kprintf("%s ", fields1[fi]);
                kprintf(" ");
                for (int fi = 0; fi < nf2; fi++)
                    if (fi != join_field-1) kprintf("%s ", fields2[fi]);
                kprintf("\n");
            }
        }
    }
}
