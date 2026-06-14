#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: du <file> [file...]\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) printf("du: %s: error\n", argv[i]);
        else printf("%llu\t%s\n", st.st_blocks / 2, argv[i]);
    }
    return 0;
}
