/* mkfifo.c — create FIFO (named pipe) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

/* Mode parsing helper */
static unsigned long parse_mode(const char *s) {
    unsigned long mode = 0;
    if (s[0] >= '0' && s[0] <= '7') {
        /* Octal string */
        while (*s >= '0' && *s <= '7') {
            mode = (mode << 3) | (*s - '0');
            s++;
        }
    } else {
        /* Default 0644 if not specified properly */
        mode = 0644;
    }
    return mode;
}

int main(int argc, char *argv[]) {
    unsigned long mode = 0666;  /* default, subject to umask */
    const char *name = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            mode = parse_mode(argv[++i]);
        } else if (argv[i][0] == '-') {
            printf("mkfifo: invalid option '%s'\n", argv[i]);
            printf("Usage: mkfifo [-m mode] <name>\n");
            return 1;
        } else {
            name = argv[i];
        }
    }

    if (!name) {
        printf("Usage: mkfifo [-m mode] <name>\n");
        return 1;
    }

    int ret = mknod(name, S_IFIFO | mode, 0);
    if (ret < 0) {
        printf("mkfifo: cannot create fifo '%s': syscall returned %d\n", name, ret);
        printf("mkfifo: mknod syscall may not be supported in this kernel\n");
        return 1;
    }

    printf("mkfifo: created '%s' (mode 0%lo)\n", name, mode);
    return 0;
}
