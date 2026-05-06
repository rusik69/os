/* cmd_test.c — Condition evaluation (test / [) */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static long to_num(const char *s) {
    long sign = 1, v = 0;
    if (*s == '-') { sign = -1; s++; }
    while (is_digit(*s)) { v = v * 10 + (*s - '0'); s++; }
    return sign * v;
}

void cmd_test(const char *args) {
    if (!args || !args[0]) {
        kprintf("false\n");
        return;
    }

    const char *p = args;
    /* Trim trailing ] if used as [ */
    char buf[256];
    strncpy(buf, args, 255);
    buf[255] = '\0';
    int len = strlen(buf);
    while (len > 0 && buf[len-1] == ' ') buf[--len] = '\0';
    if (len > 0 && buf[len-1] == ']') buf[--len] = '\0';
    while (len > 0 && buf[len-1] == ' ') buf[--len] = '\0';
    p = buf;
    while (*p == ' ') p++;

    int result = 0;

    /* Unary tests */
    if (p[0] == '-' && p[1] && p[2] == ' ') {
        char flag = p[1];
        const char *arg = p + 3;
        while (*arg == ' ') arg++;

        if (flag == 'n') {
            /* -n STRING: true if non-empty */
            result = (arg[0] != '\0');
        } else if (flag == 'z') {
            /* -z STRING: true if empty */
            result = (arg[0] == '\0');
        } else if (flag == 'f' || flag == 'e') {
            /* -f FILE / -e FILE: true if file exists */
            char path[64];
            if (arg[0] != '/') { path[0] = '/'; strncpy(path + 1, arg, 62); }
            else strncpy(path, arg, 63);
            path[63] = '\0';
            struct vfs_stat st;
            result = (vfs_stat(path, &st) == 0);
        } else if (flag == 'd') {
            /* -d PATH: true if directory */
            char path[64];
            if (arg[0] != '/') { path[0] = '/'; strncpy(path + 1, arg, 62); }
            else strncpy(path, arg, 63);
            path[63] = '\0';
            struct vfs_stat st;
            result = (vfs_stat(path, &st) == 0 && st.type == 2);
        }
    } else {
        /* Binary: arg1 op arg2 */
        char arg1[64] = {0};
        char op[8] = {0};
        char arg2[64] = {0};
        int i = 0;
        while (*p && *p != ' ' && i < 63) arg1[i++] = *p++;
        arg1[i] = '\0';
        while (*p == ' ') p++;
        i = 0;
        while (*p && *p != ' ' && i < 7) op[i++] = *p++;
        op[i] = '\0';
        while (*p == ' ') p++;
        i = 0;
        while (*p && *p != ' ' && i < 63) arg2[i++] = *p++;
        arg2[i] = '\0';

        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
            result = (strcmp(arg1, arg2) == 0);
        else if (strcmp(op, "!=") == 0)
            result = (strcmp(arg1, arg2) != 0);
        else if (strcmp(op, "-eq") == 0)
            result = (to_num(arg1) == to_num(arg2));
        else if (strcmp(op, "-ne") == 0)
            result = (to_num(arg1) != to_num(arg2));
        else if (strcmp(op, "-lt") == 0)
            result = (to_num(arg1) < to_num(arg2));
        else if (strcmp(op, "-gt") == 0)
            result = (to_num(arg1) > to_num(arg2));
        else if (strcmp(op, "-le") == 0)
            result = (to_num(arg1) <= to_num(arg2));
        else if (strcmp(op, "-ge") == 0)
            result = (to_num(arg1) >= to_num(arg2));
        else if (op[0] == '\0') {
            /* Single arg: true if non-empty */
            result = (arg1[0] != '\0');
        }
    }

    kprintf("%s\n", result ? "true" : "false");
}
