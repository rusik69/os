#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "heap.h"

/* Color codes for severity highlighting */
#define ANSI_RED     "\x1b[31m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_RESET   "\x1b[0m"

/* Check if a line contains error/warning keywords */
static int line_has_severity(const char *line, int *is_error) {
    const char *lower_keywords_err[] = {"error", "err", "fail", "bug", "panic", NULL};
    const char *upper_keywords_err[] = {"ERROR", "ERR", "FAIL", "BUG", "PANIC", NULL};
    const char *lower_keywords_warn[] = {"warning", "warn", NULL};
    const char *upper_keywords_warn[] = {"WARNING", "WARN", NULL};

    /* Check for error keywords */
    for (int i = 0; lower_keywords_err[i]; i++) {
        if (strstr(line, lower_keywords_err[i]) || strstr(line, upper_keywords_err[i])) {
            *is_error = 1;
            return 1;
        }
    }
    /* Check for warning keywords */
    for (int i = 0; lower_keywords_warn[i]; i++) {
        if (strstr(line, lower_keywords_warn[i]) || strstr(line, upper_keywords_warn[i])) {
            *is_error = 0;
            return 1;
        }
    }
    return 0;
}

void cmd_dmesg(const char *args) {
    /* Skip leading spaces */
    while (args && *args == ' ') args++;

    /* Parse flags */
    int do_clear = 0;
    int do_read_clear = 0;
    int do_clear_only = 0; /* -C: clear without reading */
    int do_color = 1;       /* default: color */
    int filter_level = -1;  /* -n level: only show messages <= this level */

    if (args && args[0] == '-') {
        int ci = 1;
        while (args[ci] && args[ci] != ' ') {
            if (args[ci] == 'c') {
                do_clear = 1;
            } else if (args[ci] == 'C') {
                do_clear_only = 1;
            } else if (args[ci] == 'n') {
                /* -n level: set log level filter */
                ci++;
                while (args[ci] == ' ') ci++;
                filter_level = 0;
                while (args[ci] >= '0' && args[ci] <= '9') {
                    filter_level = filter_level * 10 + (args[ci] - '0');
                    ci++;
                }
                if (filter_level < 0 || filter_level > 7) {
                    kprintf("dmesg: invalid log level %d (must be 0-7)\n", filter_level);
                    return;
                }
                /* Set console_loglevel for filtering */
                console_loglevel = filter_level;
            } else if (args[ci] == '-') {
                if (strcmp(args + ci, "--read-clear") == 0) {
                    do_read_clear = 1;
                    break;
                }
                if (strcmp(args + ci, "--no-color") == 0) {
                    do_color = 0;
                    break;
                }
            }
            ci++;
        }
    }

    /* -C: clear without reading */
    if (do_clear_only) {
        kprintf_dmesg_clear();
        return;
    }

    /* The ring buffer is 64 KB — allocate on heap to avoid stack pressure
     * and read the full content regardless of size. */
    char *buf = (char *)kmalloc(65536);
    if (!buf) {
        kprintf("dmesg: out of memory\n");
        return;
    }

    int n __attribute__((unused)) = kprintf_dmesg(buf, 65536);

    /* Colorize output line by line */
    if (do_color) {
        char *line_start = buf;
        char *p = buf;
        while (*p) {
            if (*p == '\n') {
                *p = '\0';
                /* Check severity of this line */
                int is_error = 0;
                if (line_has_severity(line_start, &is_error)) {
                    if (is_error)
                        kprintf(ANSI_RED "%s" ANSI_RESET "\n", line_start);
                    else
                        kprintf(ANSI_YELLOW "%s" ANSI_RESET "\n", line_start);
                } else {
                    kprintf("%s\n", line_start);
                }
                p++;
                line_start = p;
            } else {
                p++;
            }
        }
        /* Last line (if any) */
        if (line_start < p) {
            int is_error = 0;
            if (line_has_severity(line_start, &is_error)) {
                if (is_error)
                    kprintf(ANSI_RED "%s" ANSI_RESET "\n", line_start);
                else
                    kprintf(ANSI_YELLOW "%s" ANSI_RESET "\n", line_start);
            } else {
                kprintf("%s\n", line_start);
            }
        }
    } else {
        kprintf("%s", buf);
    }

    if (do_clear || do_read_clear)
        kprintf_dmesg_clear();
    kfree(buf);
}
