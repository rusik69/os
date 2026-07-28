/* mkdir.c — create directories */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

/* syscall return convention: kernel returns -(errno) on error */
#define _EEXIST (-17)

/* Forward declaration */
static int mkdir_p(char *path, int mode);

int main(int argc, char *argv[]) {
    int mode = 0777, p = 0;
    int i = 1;
    if (argc > 1 && strcmp(argv[1], "-p") == 0) { p = 1; i = 2; }
    if (i >= argc) { printf("Usage: mkdir [-p] <dir>...\n"); return 1; }
    for (; i < argc; i++) {
        if (p) {
            char path[256];
            int n;
            for (n = 0; argv[i][n] && n < (int)sizeof(path) - 1; n++)
                path[n] = argv[i][n];
            path[n] = '\0';
            if (mkdir_p(path, mode) < 0) {
                printf("mkdir: cannot create '%s'\n", argv[i]);
                return 1;
            }
        } else {
            if (mkdir(argv[i], mode) < 0) {
                printf("mkdir: cannot create '%s'\n", argv[i]);
                return 1;
            }
        }
    }
    return 0;
}

/*
 * Helper: mkdir -p semantics — create each path component.
 * Walks through the path and calls mkdir() for each component.
 */
static int mkdir_p(char *path, int mode)
{
    char *p = path;
    int ret;

    /* Handle absolute paths: preserve the leading '/' */
    if (*p == '/')
        p++;

    while (*p) {
        /* Advance to the next '/' separator */
        while (*p && *p != '/')
            p++;

        /* Temporarily terminate at this component */
        char saved = *p;
        *p = '\0';

        /* Try to create this path prefix */
        ret = mkdir(path, mode);
        /* EEXIST is not an error for -p */
        if (ret < 0 && ret != _EEXIST) {
            *p = saved;
            return ret;
        }

        /* Restore separator and advance */
        *p = saved;
        if (saved == '/')
            p++;
    }

    return 0;
}
