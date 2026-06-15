/* chcon.c — change security context of a file */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: chcon CONTEXT FILE...\n");
        return 1;
    }
    const char *context = argv[1];
    int ret = 0;
    for (int i = 2; i < argc; i++) {
        /* Try writing context to file's xattr via /proc/self/attr */
        char procpath[256];
        snprintf(procpath, sizeof(procpath), "/proc/self/attr/%s", argv[i]);
        int fd = open(procpath, O_WRONLY, 0);
        if (fd >= 0) {
            write(fd, context, strlen(context));
            write(fd, "\n", 1);
            close(fd);
            printf("chcon: set context '%s' on '%s'\n", context, argv[i]);
        } else {
            /* Fallback: try /proc/attr/current (process-wide) */
            fd = open("/proc/attr/current", O_WRONLY, 0);
            if (fd >= 0) {
                write(fd, context, strlen(context));
                write(fd, "\n", 1);
                close(fd);
                printf("chcon: set process context '%s' for '%s'\n", context, argv[i]);
            } else {
                printf("chcon: cannot set context on '%s' (no xattr support)\n", argv[i]);
                ret = 1;
            }
        }
    }
    return ret;
}
