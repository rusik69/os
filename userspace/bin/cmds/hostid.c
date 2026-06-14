/* hostid.c — print host identifier (hex hash of hostname) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    char hostname[65];
    if (gethostname(hostname, 64) < 0) {
        printf("hostid: cannot get hostname\n");
        return 1;
    }
    hostname[64] = '\0';
    /* Simple hash of hostname to produce a 32-bit ID */
    unsigned long hash = 0;
    char *p = hostname;
    while (*p) {
        hash = hash * 31 + (unsigned char)(*p);
        p++;
    }
    printf("%08lx\n", hash);
    return 0;
}
