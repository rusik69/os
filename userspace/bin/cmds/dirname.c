/* dirname.c — strip filename from pathname */

#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: dirname <path>\n");
        return 1;
    }
    const char *path = argv[1];
    unsigned long len = strlen(path);
    long last_slash = -1;
    for (unsigned long i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = (long)i;
    }
    if (last_slash < 0) {
        write(1, ".", 1);
    } else if (last_slash == 0) {
        write(1, "/", 1);
    } else {
        write(1, path, (unsigned long)last_slash);
    }
    write(1, "\n", 1);
    return 0;
}
