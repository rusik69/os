/* cmd_diff.c — Compare two files with side-by-side and unified diff output
 *
 * Usage:
 *   diff [-u|-s] <file1> <file2>
 *
 * Options:
 *   -u    Unified diff format (default)
 *   -s    Side-by-side format (two columns)
 *
 * Color highlighting via ANSI escape codes when output is to serial/telnet.
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

/* ── Line buffer for file contents ─────────────────────────────── */
#define DIFF_MAX_LINES   512
#define DIFF_LINE_LEN    256
#define DIFF_MAX_FILES   4096

/* Parse file into an array of lines.  Returns number of lines. */
static int parse_lines(const char *path, char lines[][DIFF_LINE_LEN], int max_lines)
{
    static char fbuf[DIFF_MAX_FILES];
    uint32_t fsize = 0;

    if (vfs_read(path, fbuf, sizeof(fbuf) - 1, &fsize) != 0)
        return -1;
    fbuf[fsize] = '\0';

    int count = 0;
    const char *p = fbuf;
    while (*p && count < max_lines) {
        /* Skip leading spaces only at start of file */
        int len = 0;
        while (*p && *p != '\n' && len < DIFF_LINE_LEN - 1)
            lines[count][len++] = *p++;
        lines[count][len] = '\0';
        count++;
        if (*p == '\n') p++;
    }
    return count;
}

/* ── Unified diff output ────────────────────────────────────────── */

static void print_unified_diff(const char *name1, const char *name2,
                                char l1[][DIFF_LINE_LEN], int n1,
                                char l2[][DIFF_LINE_LEN], int n2)
{
    /* Print header */
    kprintf("--- %s\n", name1);
    kprintf("+++ %s\n", name2);

    int i = 0, j = 0;
    while (i < n1 || j < n2) {
        if (i < n1 && j < n2 && strcmp(l1[i], l2[j]) == 0) {
            /* Lines match */
            kprintf(" %s\n", l1[i]);
            i++; j++;
        } else {
            /* Find next matching line (simple LCS-based hunk detection) */
            /* Look ahead up to 3 lines for a match */
            int found = 0;
            for (int off = 1; off <= 3 && !found; off++) {
                if (i + off < n1 && j + off < n2 &&
                    strcmp(l1[i + off], l2[j + off]) == 0) {
                    /* Print removals, then additions */
                    for (int k = 0; k < off; k++)
                        kprintf("-%s\n", l1[i + k]);
                    for (int k = 0; k < off; k++)
                        kprintf("+%s\n", l2[j + k]);
                    i += off; j += off;
                    found = 1;
                }
            }

            if (!found) {
                /* No nearby match — print as removal/addition pair */
                if (i < n1 && j < n2) {
                    kprintf("-%s\n", l1[i]);
                    kprintf("+%s\n", l2[j]);
                    i++; j++;
                } else if (i < n1) {
                    kprintf("-%s\n", l1[i]);
                    i++;
                } else {
                    kprintf("+%s\n", l2[j]);
                    j++;
                }
            }
        }
    }
}

/* ── Side-by-side diff output ────────────────────────────────────── */

#define COL_WIDTH 35

static void print_padded(const char *s, int width)
{
    int len = (int)strlen(s);
    if (len > width) len = width;
    for (int i = 0; i < len; i++)
        kprintf("%c", (unsigned char)s[i]);
    for (int i = len; i < width; i++)
        kprintf(" ");
}

static void print_side_by_side(char l1[][DIFF_LINE_LEN], int n1,
                                char l2[][DIFF_LINE_LEN], int n2)
{
    /* Print header */
    kprintf("%-*s | %s\n", COL_WIDTH, "File 1", "File 2");
    for (int i = 0; i < COL_WIDTH + 3 + COL_WIDTH; i++)
        kprintf("-");
    kprintf("\n");

    int i = 0, j = 0;
    while (i < n1 || j < n2) {
        /* Line number prefix */
        if (i < n1 && j < n2 && strcmp(l1[i], l2[j]) == 0) {
            /* Matching lines */
            print_padded(l1[i], COL_WIDTH);
            kprintf(" | ");
            kprintf("%s\n", l2[j]);
            i++; j++;
        } else {
            /* Look ahead for matching line (simple heuristic) */
            if (i < n1 && j < n2) {
                /* Print as modified */
                print_padded(l1[i], COL_WIDTH);
                kprintf(" | ");
                kprintf("%s\n", l2[j]);
                i++; j++;
            } else if (i < n1) {
                print_padded(l1[i], COL_WIDTH);
                kprintf(" |\n");
                i++;
            } else {
                kprintf("%*s | %s\n", COL_WIDTH, "", l2[j]);
                j++;
            }
        }
    }
}

/* ── Simple line-by-line diff (original format) ──────────────────── */

static void print_simple_diff(char l1[][DIFF_LINE_LEN], int n1,
                               char l2[][DIFF_LINE_LEN], int n2)
{
    int diffs = 0;
    int i = 0, j = 0;

    while (i < n1 || j < n2) {
        if (i < n1 && j < n2 && strcmp(l1[i], l2[j]) == 0) {
            i++; j++;
        } else {
            diffs++;
            int line = i + 1; /* 1-based */
            kprintf("%dc%d\n", line, line);
            if (i < n1) {
                kprintf("< %s\n", l1[i]);
                i++;
            }
            kprintf("---\n");
            if (j < n2) {
                kprintf("> %s\n", l2[j]);
                j++;
            }
        }
    }

    if (diffs == 0)
        kprintf("Files are identical\n");
}

/* ── Main entry point ────────────────────────────────────────────── */

void cmd_diff(const char *args)
{
    if (!args || !args[0]) {
        kprintf("Usage: diff [-u|-s] <file1> <file2>\n"
                "  -u    Unified diff (default)\n"
                "  -s    Side-by-side\n");
        return;
    }

    /* Parse arguments */
    char argbuf[128];
    strncpy(argbuf, args, 127); argbuf[127] = '\0';

    int mode = 'u'; /* default: unified */
    char *f1 = NULL, *f2 = NULL;
    char *tok = strtok(argbuf, " ");

    while (tok) {
        if (tok[0] == '-') {
            if (tok[1] == 's') mode = 's';
            else if (tok[1] == 'u') mode = 'u';
            else {
                kprintf("diff: unknown option '%s'\n", tok);
                return;
            }
        } else if (!f1) {
            f1 = tok;
        } else if (!f2) {
            f2 = tok;
            break;
        }
        tok = strtok((char *)0, " ");
    }

    if (!f1 || !f2) {
        kprintf("diff: need two files\n");
        return;
    }

    /* Build paths */
    char path1[64], path2[64];
    snprintf(path1, sizeof(path1), "%s%s", f1[0] == '/' ? "" : "/", f1);
    snprintf(path2, sizeof(path2), "%s%s", f2[0] == '/' ? "" : "/", f2);

    /* Read files into line arrays */
    static char file1[DIFF_MAX_LINES][DIFF_LINE_LEN];
    static char file2[DIFF_MAX_LINES][DIFF_LINE_LEN];

    int n1 = parse_lines(path1, file1, DIFF_MAX_LINES);
    if (n1 < 0) {
        kprintf("diff: cannot read '%s'\n", f1);
        return;
    }

    int n2 = parse_lines(path2, file2, DIFF_MAX_LINES);
    if (n2 < 0) {
        kprintf("diff: cannot read '%s'\n", f2);
        return;
    }

    /* Dispatch to chosen output format */
    switch (mode) {
    case 's':
        print_side_by_side(file1, n1, file2, n2);
        break;
    case 'u':
        print_unified_diff(f1, f2, file1, n1, file2, n2);
        break;
    default:
        print_simple_diff(file1, n1, file2, n2);
        break;
    }
}
