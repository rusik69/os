/* ip.c — IP routing (stub) */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: ip <command> [args...]\n");
        printf("commands: link, addr, route, neigh\n");
        return 1;
    }
    (void)argv;
    printf("ip: not supported\n");
    return 1;
}
