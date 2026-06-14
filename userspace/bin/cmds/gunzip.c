/* gunzip.c — gunzip (stub, just copy stdin to stdout) */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    char buf[4096];
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    return 0;
}
