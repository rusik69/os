/* chrt.c — change scheduling policy */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: chrt [options] <priority> <command> [args...]\n");
        printf("       chrt -p [priority] <pid>\n");
        return 1;
    }
    (void)argv;
    printf("chrt: scheduling policy changes not supported\n");
    return 1;
}
