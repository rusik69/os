/* login.c — simple login prompt (stub) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("HermesOS login: ");
    /* In a real system, would read password, verify, start shell */
    printf("\n");
    printf("login: not implemented in this build\n");
    printf("(no authentication — running as uid=%d)\n", getuid());
    return 0;
}
