/* cmd_test.c — Condition evaluation (test / [)
 *
 * POSIX-compatible test command with full operator support:
 *
 * File tests: -f, -d, -e, -x, -w, -r, -s, -L, -h
 * String tests: =, ==, !=, -z, -n, <, >
 * Integer tests: -eq, -ne, -lt, -le, -gt, -ge
 * Logical operators: !, -a, -o
 *
 * Usage:
 *   test EXPRESSION
 *   [ EXPRESSION ]
 *
 * Returns 0 (true) or 1 (false).  Exit status reflects the result.
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

/* ── Helpers ────────────────────────────────────────────────────────── */

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static long to_num(const char *s) {
    long sign = 1, v = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (is_digit(*s)) { v = v * 10 + (*s - '0'); s++; }
    return sign * v;
}

/*
 * Tokenise the argument string into words separated by spaces.
 * Returns the number of tokens (max TOKEN_MAX).
 */
#define TOKEN_MAX 32

static int tokenise(const char *str, char tokens[TOKEN_MAX][64]) {
    int count = 0;
    const char *p = str;
    while (*p && count < TOKEN_MAX) {
        while (*p == ' ') p++;
        if (!*p) break;
        int i = 0;
        while (*p && *p != ' ' && i < 63)
            tokens[count][i++] = *p++;
        tokens[count][i] = '\0';
        count++;
    }
    return count;
}

/*
 * Evaluate a file test.
 * Returns 1 if the condition is true, 0 otherwise.
 */
static int file_test(const char *flag, const char *path) {
    /* Build absolute path if needed */
    char full_path[128];
    if (path[0] != '/') {
        full_path[0] = '/';
        strncpy(full_path + 1, path, sizeof(full_path) - 2);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        strncpy(full_path, path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    }

    struct vfs_stat st;
    int exists = (vfs_stat(full_path, &st) == 0);

    switch (flag[0]) {
    case 'e': /* -e: exists (any type) */
        return exists;

    case 'f': /* -f: regular file */
        return exists && (st.type == FS_TYPE_FILE);

    case 'd': /* -d: directory */
        return exists && (st.type == FS_TYPE_DIR);

    case 's': /* -s: non-empty file */
        return exists && (st.size > 0);

    case 'x': /* -x: executable (has execute bit set) */
        return exists && (st.mode & 0111);

    case 'w': /* -w: writable (has write bit set) */
        return exists && (st.mode & 0222);

    case 'r': /* -r: readable (has read bit set) */
        return exists && (st.mode & 0444);

    case 'L': /* -L: symbolic link */
    case 'h': /* -h: symbolic link (alias) */
        return exists && (st.type == FS_TYPE_LINK);

    default:
        return 0;
    }
}

/* ── Main entry point ───────────────────────────────────────────────── */

void cmd_test(const char *args) {
    if (!args || !args[0]) {
        /* No args: POSIX says false (exit 1) */
        shell_set_exit_status(1);
        return;
    }

    /* Trim trailing ] if used as [ */
    char buf[512];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int len = (int)strlen(buf);

    /* Strip trailing spaces and optional ] */
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = '\0';
    if (len > 0 && buf[len - 1] == ']')
        buf[--len] = '\0';
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = '\0';

    /* Tokenise the expression */
    char tokens[TOKEN_MAX][64];
    int tok_count = tokenise(buf, tokens);

    if (tok_count == 0) {
        shell_set_exit_status(1);
        return;
    }

    int result = 0;
    int pos = 0;

    /* ── Handle leading ! (logical NOT) ───────────────────────────── */
    int invert = 0;
    while (pos < tok_count && strcmp(tokens[pos], "!") == 0) {
        invert = !invert;
        pos++;
    }

    /* ── Evaluate the expression ──────────────────────────────────── */
    if (pos >= tok_count) {
        /* Just "!" with nothing after — true (inverted: "!" = false exit 1) */
        result = 0;
    }
    /* Unary: -<flag> <arg>  e.g., -f /path, -n "string", -z "string" */
    else if (tokens[pos][0] == '-' && (tokens[pos][1] == 'f' ||
                                        tokens[pos][1] == 'd' ||
                                        tokens[pos][1] == 'e' ||
                                        tokens[pos][1] == 'x' ||
                                        tokens[pos][1] == 'w' ||
                                        tokens[pos][1] == 'r' ||
                                        tokens[pos][1] == 's' ||
                                        tokens[pos][1] == 'L' ||
                                        tokens[pos][1] == 'h') &&
             tokens[pos][2] == '\0') {
        /* File test: -<flag> <path> */
        if (pos + 1 < tok_count) {
            result = file_test(&tokens[pos][1], tokens[pos + 1]);
            pos += 2;
        }
    }
    else if (strcmp(tokens[pos], "-n") == 0) {
        /* -n STRING: true if non-empty */
        if (pos + 1 < tok_count) {
            result = (tokens[pos + 1][0] != '\0');
            pos += 2;
        }
    }
    else if (strcmp(tokens[pos], "-z") == 0) {
        /* -z STRING: true if empty */
        if (pos + 1 < tok_count) {
            result = (tokens[pos + 1][0] == '\0');
            pos += 2;
        }
    }
    else if (tok_count - pos == 1) {
        /* Single arg: true if non-empty string */
        result = (tokens[pos][0] != '\0');
        pos++;
    }
    else if (pos + 2 < tok_count) {
        /* Binary: arg1 OP arg2 */
        const char *op = tokens[pos + 1];
        const char *arg1 = tokens[pos];
        const char *arg2 = tokens[pos + 2];

        /* String comparison */
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            result = (strcmp(arg1, arg2) == 0);
        } else if (strcmp(op, "!=") == 0) {
            result = (strcmp(arg1, arg2) != 0);
        } else if (strcmp(op, "<") == 0) {
            /* Lexicographic less-than (ASCII order) */
            result = (strcmp(arg1, arg2) < 0);
        } else if (strcmp(op, ">") == 0) {
            /* Lexicographic greater-than (ASCII order) */
            result = (strcmp(arg1, arg2) > 0);
        }
        /* Integer comparison */
        else if (strcmp(op, "-eq") == 0) {
            result = (to_num(arg1) == to_num(arg2));
        } else if (strcmp(op, "-ne") == 0) {
            result = (to_num(arg1) != to_num(arg2));
        } else if (strcmp(op, "-lt") == 0) {
            result = (to_num(arg1) < to_num(arg2));
        } else if (strcmp(op, "-le") == 0) {
            result = (to_num(arg1) <= to_num(arg2));
        } else if (strcmp(op, "-gt") == 0) {
            result = (to_num(arg1) > to_num(arg2));
        } else if (strcmp(op, "-ge") == 0) {
            result = (to_num(arg1) >= to_num(arg2));
        } else {
            /* Unknown operator — treat as false */
            result = 0;
        }
        pos += 3;
    }

    /* ── Handle subsequent logical operators (-a, -o) ─────────────── */
    while (pos < tok_count) {
        if (strcmp(tokens[pos], "-a") == 0) {
            /* AND: need another expression */
            pos++;
            int rhs = 0;
            int sub_invert = 0;

            /* Handle ! after -a */
            while (pos < tok_count && strcmp(tokens[pos], "!") == 0) {
                sub_invert = !sub_invert;
                pos++;
            }

            if (pos < tok_count) {
                if (tokens[pos][0] == '-' && (tokens[pos][1] == 'f' ||
                                               tokens[pos][1] == 'd' ||
                                               tokens[pos][1] == 'e' ||
                                               tokens[pos][1] == 'x' ||
                                               tokens[pos][1] == 'w' ||
                                               tokens[pos][1] == 'r' ||
                                               tokens[pos][1] == 's' ||
                                               tokens[pos][1] == 'L' ||
                                               tokens[pos][1] == 'h') &&
                    tokens[pos][2] == '\0' && pos + 1 < tok_count) {
                    rhs = file_test(&tokens[pos][1], tokens[pos + 1]);
                    pos += 2;
                } else if (strcmp(tokens[pos], "-n") == 0 && pos + 1 < tok_count) {
                    rhs = (tokens[pos + 1][0] != '\0');
                    pos += 2;
                } else if (strcmp(tokens[pos], "-z") == 0 && pos + 1 < tok_count) {
                    rhs = (tokens[pos + 1][0] == '\0');
                    pos += 2;
                } else {
                    /* Single arg: true if non-empty */
                    rhs = (tokens[pos][0] != '\0');
                    pos++;
                }
            }
            if (sub_invert) rhs = !rhs;
            result = result && rhs;
        } else if (strcmp(tokens[pos], "-o") == 0) {
            /* OR: need another expression */
            pos++;
            int rhs = 0;
            int sub_invert = 0;

            while (pos < tok_count && strcmp(tokens[pos], "!") == 0) {
                sub_invert = !sub_invert;
                pos++;
            }

            if (pos < tok_count) {
                if (tokens[pos][0] == '-' && (tokens[pos][1] == 'f' ||
                                               tokens[pos][1] == 'd' ||
                                               tokens[pos][1] == 'e' ||
                                               tokens[pos][1] == 'x' ||
                                               tokens[pos][1] == 'w' ||
                                               tokens[pos][1] == 'r' ||
                                               tokens[pos][1] == 's' ||
                                               tokens[pos][1] == 'L' ||
                                               tokens[pos][1] == 'h') &&
                    tokens[pos][2] == '\0' && pos + 1 < tok_count) {
                    rhs = file_test(&tokens[pos][1], tokens[pos + 1]);
                    pos += 2;
                } else if (strcmp(tokens[pos], "-n") == 0 && pos + 1 < tok_count) {
                    rhs = (tokens[pos + 1][0] != '\0');
                    pos += 2;
                } else if (strcmp(tokens[pos], "-z") == 0 && pos + 1 < tok_count) {
                    rhs = (tokens[pos + 1][0] == '\0');
                    pos += 2;
                } else {
                    rhs = (tokens[pos][0] != '\0');
                    pos++;
                }
            }
            if (sub_invert) rhs = !rhs;
            result = result || rhs;
        } else {
            /* Unexpected token — stop parsing */
            break;
        }
    }

    if (invert) result = !result;

    shell_set_exit_status(result ? 0 : 1);
}
