/* iconv.c — character set conversion (stub) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("usage: iconv -f <from> -t <to> [file]\n");
        return 1;
    }
    printf("iconv: not yet implemented\n");
    return 1;
}
