/* touch.c — create or update file timestamps using utimensat */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: touch <file>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            printf("touch: cannot create '%s'\n", argv[i]);
            continue;
        }
        /* Update timestamps via utimensat with NULL times = set to now */
        if (utimensat(AT_FDCWD, argv[i], (void *)0, 0) < 0) {
            /* Ignore errors if utimensat fails */
        }
        close(fd);
    }
    return 0;
}
