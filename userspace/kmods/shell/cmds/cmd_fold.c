/* cmd_fold.c -- Fold (wrap) long lines */
#include "libc.h"
#include "printf.h"
#include "shell_cmds.h"
#include "stdlib.h"
#include "string.h"
#include "types.h"

int cmd_fold(int argc, char **argv) {
    int width = 80;
    int break_at_spaces = 0;
    int optidx = 1;

    while (optidx < argc && argv[optidx][0] == '-') {
        if (strcmp(argv[optidx], "-w") == 0) {
            if (optidx + 1 >= argc) {
                kprintf("fold: -w requires an argument\n");
                return 1;
            }
            width = atoi(argv[optidx + 1]);
            if (width < 1)
                width = 1;
            optidx += 2;
        } else if (strcmp(argv[optidx], "-s") == 0) {
            break_at_spaces = 1;
            optidx++;
        } else if (strcmp(argv[optidx], "--") == 0) {
            optidx++;
            break;
        } else {
            kprintf("fold: unknown option '%s'\n", argv[optidx]);
            return 1;
        }
    }

    /* Read input */
    static char fbuf[32768];
    uint32_t fsize = 0;

    if (optidx < argc) {
        char path[64];
        const char *fn = argv[optidx];
        if (fn[0] != '/') {
            path[0] = '/';
            strncpy(path + 1, fn, 61);
            path[62] = '\0';
        } else {
            strncpy(path, fn, 63);
            path[63] = '\0';
        }
        int pl = (int)strlen(path);
        while (pl > 0 && path[pl - 1] == ' ')
            path[--pl] = '\0';
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
        if (*p == '\n')
            p++;

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
                    for (int k = pos; k < li; k++)
                        kprintf("%c", line[k]);
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
                    for (int k = pos; k < break_point; k++)
                        kprintf("%c", line[k]);
                    kprintf("\n");
                    pos = break_point + 1; /* skip the space */
                } else {
                    /* No space found in range, break at width */
                    for (int k = pos; k < pos + width && k < li; k++)
                        kprintf("%c", line[k]);
                    kprintf("\n");
                    pos += width;
                }
            } else {
                /* Hard break at width */
                for (int k = pos; k < pos + width && k < li; k++)
                    kprintf("%c", line[k]);
                kprintf("\n");
                pos += width;
            }
        }
    }
    return 0;
}
