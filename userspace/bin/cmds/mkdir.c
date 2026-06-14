/* mkdir.c — create directories */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: mkdir <dir>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            printf("mkdir: failed to create '%s'\n", argv[i]);
            return 1;
        }
    }
    return 0;
}
