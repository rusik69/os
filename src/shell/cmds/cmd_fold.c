/* cmd_fold.c -- Fold (wrap) long lines */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_fold(int argc, char **argv) {
    int width = 80;
    int break_at_spaces = 0;
    int optind = 1;

    while (optind < argc && argv[optind][0] == '-') {
        if (strcmp(argv[optind], "-w") == 0) {
            if (optind + 1 >= argc) {
                kprintf("fold: -w requires an argument\n");
                return 1;
            }
            width = atoi(argv[optind + 1]);
            if (width < 1) width = 1;
            optind += 2;
        } else if (strcmp(argv[optind], "-s") == 0) {
            break_at_spaces = 1;
            optind++;
        } else if (strcmp(argv[optind], "--") == 0) {
            optind++; break;
        } else {
            kprintf("fold: unknown option '%s'\n", argv[optind]);
            return 1;
        }
    }

    /* Read input */
    static char fbuf[32768];
    uint32_t fsize = 0;

    if (optind < argc) {
        char path[64];
        const char *fn = argv[optind];
        if (fn[0] != '/') { path[0] = '/'; strncpy(path + 1, fn, 61); path[62] = '\0'; }
        else { strncpy(path, fn, 63); path[63] = '\0'; }
        int pl = (int)strlen(path);
        while (pl > 0 && path[pl - 1] == ' ') path[--pl] = '\0';
        if (vfs_read(path, fbuf, (uint32_t)(sizeof(fbuf) - 1), &fsize) != 0) {
            kprintf("fold: cannot read '%s'\n", fn);
            return 1;
        }
        fbuf[fsize] = '\0';
    } else {
        if (!shell_has_stdin()) {
            kprintf("Usage: fold [-w WIDTH] [-s] [file]\n");
            return 1;
        }
        fsize = (uint32_t)shell_stdin_read(fbuf, (int)sizeof(fbuf) - 1);
        fbuf[fsize] = '\0';
    }

    /* Process line by line using a line buffer */
    char *p = fbuf;
    while (*p) {
        /* Read one line from input */
        static char line[4096];
        int li = 0;
        while (*p && *p != '\n' && li < 4095)
            line[li++] = *p++;
        line[li] = '\0';
        if (*p == '\n') p++;

        /* If line fits within width, output as-is */
        if (li <= width) {
            kprintf("%s\n", line);
            continue;
        }

        /* Fold the line */
        int pos = 0;
        while (pos < li) {
            if (break_at_spaces) {
                /* Try to find a space within width */
                int end = pos + width;
                if (end >= li) {
                    /* Last chunk */
                    for (int k = pos; k < li; k++) kprintf("%c", line[k]);
                    kprintf("\n");
                    break;
                }
                /* Look for last space before width boundary */
                int break_point = -1;
                for (int k = pos; k < end && k < li; k++) {
                    if (line[k] == ' ')
                        break_point = k;
                }
                if (break_point > pos) {
                    /* Break at the space */
                    for (int k = pos; k < break_point; k++) kprintf("%c", line[k]);
                    kprintf("\n");
                    pos = break_point + 1; /* skip the space */
                } else {
                    /* No space found in range, break at width */
                    for (int k = pos; k < pos + width && k < li; k++) kprintf("%c", line[k]);
                    kprintf("\n");
                    pos += width;
                }
            } else {
                /* Hard break at width */
                for (int k = pos; k < pos + width && k < li; k++) kprintf("%c", line[k]);
                kprintf("\n");
                pos += width;
            }
        }
    }
    return 0;
}
