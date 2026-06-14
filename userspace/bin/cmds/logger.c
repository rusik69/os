/* logger.c — log message to syslog (print to stderr) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int i = 1;
    if (argc > 1 && argv[1][0] == '-') {
        /* skip options */
        i++;
    }
    if (i >= argc) {
        /* read from stdin */
        char buf[4096];
        long n;
        while ((n = read(0, buf, sizeof(buf))) > 0) {
            write(2, buf, n);
        }
        return 0;
    }
    for (; i < argc; i++) {
        if (i > 1 || (argc > 1 && argv[1][0] == '-' && i > 2)) write(2, " ", 1);
        write(2, argv[i], strlen(argv[i]));
    }
    write(2, "\n", 1);
    return 0;
}
