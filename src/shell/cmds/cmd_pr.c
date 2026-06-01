/* cmd_pr.c -- Paginate files for printing (headers, page numbers) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_pr(int argc, char **argv) {
    int page_size = 66;
    int header_cols = 1;
    int optind = 1;

    while (optind < argc && argv[optind][0] == '-') {
        if (strcmp(argv[optind], "-l") == 0) {
            if (optind + 1 >= argc) {
                kprintf("pr: -l requires an argument\n");
                return 1;
            }
            page_size = atoi(argv[optind + 1]);
            if (page_size < 1) page_size = 1;
            optind += 2;
        } else if (strcmp(argv[optind], "--") == 0) {
            optind++; break;
        } else {
            kprintf("pr: unknown option '%s'\n", argv[optind]);
            return 1;
        }
    }

    /* We may read from stdin or a file */
    static char fbuf[32768];
    uint32_t fsize = 0;
    const char *filename = "stdin";

    if (optind < argc) {
        char path[64];
        const char *fn = argv[optind];
        filename = fn;
        if (fn[0] != '/') { path[0] = '/'; strncpy(path + 1, fn, 61); path[62] = '\0'; }
        else { strncpy(path, fn, 63); path[63] = '\0'; }
        int pl = (int)strlen(path);
        while (pl > 0 && path[pl - 1] == ' ') path[--pl] = '\0';
        if (vfs_read(path, fbuf, (uint32_t)(sizeof(fbuf) - 1), &fsize) != 0) {
            kprintf("pr: cannot read '%s'\n", fn);
            return 1;
        }
        fbuf[fsize] = '\0';
    } else {
        if (!shell_has_stdin()) {
            kprintf("Usage: pr [-l PAGESIZE] [file]\n");
            return 1;
        }
        fsize = (uint32_t)shell_stdin_read(fbuf, (int)sizeof(fbuf) - 1);
        fbuf[fsize] = '\0';
    }

    /* Get current time for header */
    struct libc_rtc_time tm;
    int has_time = (libc_rtc_get_time(&tm) == 0);

    /* Page numbering */
    int page_num = 1;
    int line_on_page = 0;

    /* Write header */
    void write_header(void) {
        if (has_time) {
            kprintf("%04d-%02d-%02d %02d:%02d  %s  Page %d\n\n",
                    tm.year, tm.month, tm.day,
                    tm.hour, tm.minute,
                    filename, page_num);
        } else {
            kprintf("%s  Page %d\n\n", filename, page_num);
        }
        line_on_page = 2; /* header + blank line */
    }

    write_header();

    /* Output each line */
    char *p = fbuf;
    while (*p) {
        char line[4096];
        int li = 0;
        while (*p && *p != '\n' && li < 4095)
            line[li++] = *p++;
        line[li] = '\0';
        if (*p == '\n') p++;

        kprintf("%s\n", line);
        line_on_page++;

        if (line_on_page >= page_size) {
            /* Form feed */
            kprintf("\f");
            page_num++;
            write_header();
        }
    }
    return 0;
}
