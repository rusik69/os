/* basename.c — strip directory from pathname */

#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: basename <path>\n");
        return 1;
    }
    const char *path = argv[1];
    unsigned long len = strlen(path);
    long last_slash = -1;
    for (unsigned long i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = (long)i;
    }
    const char *base = path + last_slash + 1;
    write(1, base, strlen(base));
    write(1, "\n", 1);
    return 0;
}
