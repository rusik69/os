/* mkfifo.c — create FIFO (named pipe) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

/* Syscall number */
#define SYS_MKNOD 271

/* Inline syscall for mknod(path, mode, dev) */
static int mknod_call(const char *path, unsigned int mode, unsigned long dev) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_MKNOD),
          "D"((long)path),
          "S"((long)mode),
          "d"((long)dev)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

/* Mode parsing helper */
static unsigned long parse_mode(const char *s) {
    unsigned long mode = 0;
    if (s[0] >= '0' && s[0] <= '7') {
        while (*s >= '0' && *s <= '7') {
            mode = (mode << 3) | (*s - '0');
            s++;
        }
    } else {
        mode = 0644;
    }
    return mode;
}

int main(int argc, char *argv[]) {
    unsigned long mode = 0666;
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

    /* Use inline mknod syscall with S_IFIFO */
    int ret = mknod_call(name, S_IFIFO | mode, 0);
    if (ret < 0) {
        printf("mkfifo: cannot create fifo '%s'\n", name);
        return 1;
    }

    printf("mkfifo: created '%s' (mode 0%lo)\n", name, mode);
    return 0;
}
