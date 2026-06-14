/* col.c — filter reverse line feeds (pass-through) */
#include "unistd.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    char buf[4096];
    long n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
    }
    return 0;
}
