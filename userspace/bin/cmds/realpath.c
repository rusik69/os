/* realpath.c — print resolved absolute path (stub: print argument) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: realpath <path>\n"); return 1; }
    /* For now, just print the argument (stub) */
    printf("%s\n", argv[1]);
    return 0;
}
