/* cmd_expand.c -- Convert tabs to spaces */
#include "libc.h"
#include "printf.h"
#include "shell_cmds.h"
#include "stdlib.h"
#include "string.h"
#include "types.h"

int cmd_expand(int argc, char **argv) {
    int tabstop = 8;
    int optidx = 1;

    while (optidx < argc && argv[optidx][0] == '-') {
        if (strcmp(argv[optidx], "-t") == 0) {
            if (optidx + 1 >= argc) {
                kprintf("expand: -t requires an argument\n");
                return 1;
            }
            tabstop = atoi(argv[optidx + 1]);
            if (tabstop < 1)
                tabstop = 1;
            optidx += 2;
        } else if (strcmp(argv[optidx], "--") == 0) {
            optidx++;
            break;
        } else {
            kprintf("expand: unknown option '%s'\n", argv[optidx]);
            return 1;
        }
    }

    /* Determine input source */
    const char *input = NULL;
    static char fbuf[4096];
    uint32_t fsize = 0;

    if (optidx < argc) {
        /* Read from file */
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
        if (vfs_read(path, fbuf, 4095, &fsize) != 0) {
            kprintf("expand: cannot read '%s'\n", fn);
            return 1;
        }
        fbuf[fsize] = '\0';
        input = fbuf;
    } else {
        /* Read from stdin */
        if (!shell_has_stdin()) {
            kprintf("Usage: expand [-t N] [file]\n");
            return 1;
        }
        fsize = (uint32_t)shell_stdin_read(fbuf, 4095);
        fbuf[fsize] = '\0';
        input = fbuf;
    }

    /* Process character by character */
    int col = 0;
    for (uint32_t i = 0; i < fsize; i++) {
        char c = input[i];
        if (c == '\t') {
            int spaces = tabstop - (col % tabstop);
            for (int s = 0; s < spaces; s++) {
                kprintf(" ");
                col++;
            }
        } else if (c == '\n') {
            kprintf("\n");
            col = 0;
        } else {
            kprintf("%c", c);
            col++;
        }
    }
    return 0;
}
