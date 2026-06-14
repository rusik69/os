/* link.c — create a hard link (stub) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 3) {
        printf("usage: link <existing> <new>\n");
        return 1;
    }
    printf("link: not yet implemented\n");
    return 1;
}
