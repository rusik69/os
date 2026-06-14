/* which.c — search PATH for commands */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: which <command>...\n");
        return 1;
    }
    /* Get PATH from environment */
    char *path = 0;
    int fd = open("/proc/self/environ", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            char *p = buf;
            while (*p) {
                if (strncmp(p, "PATH=", 5) == 0) {
                    path = p + 5;
                    break;
                }
                while (*p) p++;
                p++;
            }
        }
        close(fd);
    }
    if (!path) {
        printf("which: PATH not set\n");
        return 1;
    }
    int found = 0;
    for (int i = 1; i < argc; i++) {
        /* Check if command contains a slash — just check directly */
        if (strchr(argv[i], '/')) {
            struct stat st;
            if (stat(argv[i], &st) >= 0) {
                printf("%s\n", argv[i]);
                found = 1;
            }
            continue;
        }
        /* Search PATH */
        char pathenv[1024];
        int pn = 0;
        for (int j = 0; path[j] && pn < 1020; j++) {
            if (path[j] == ':') {
                pathenv[pn] = 0;
                char full[1048];
                int k;
                for (k = 0; pathenv[k]; k++) full[k] = pathenv[k];
                full[k++] = '/';
                for (int l = 0; argv[i][l]; l++) full[k++] = argv[i][l];
                full[k] = 0;
                struct stat st;
                if (stat(full, &st) >= 0) {
                    printf("%s\n", full);
                    found = 1;
                    break;
                }
                pn = 0;
            } else {
                pathenv[pn++] = path[j];
            }
        }
        /* Last entry (after last ':') */
        if (pn > 0) {
            pathenv[pn] = 0;
            char full[1048];
            int k;
            for (k = 0; pathenv[k]; k++) full[k] = pathenv[k];
            full[k++] = '/';
            for (int l = 0; argv[i][l]; l++) full[k++] = argv[i][l];
            full[k] = 0;
            struct stat st;
            if (stat(full, &st) >= 0) {
                printf("%s\n", full);
                found = 1;
            }
        }
    }
    return found ? 0 : 1;
}
