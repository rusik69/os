/* unlink.c — call the unlink syscall wrapper */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: unlink <file>\n");
        return 1;
    }
    if (unlink(argv[1]) < 0) {
        printf("unlink: cannot unlink '%s'\n", argv[1]);
        return 1;
    }
    return 0;
}
