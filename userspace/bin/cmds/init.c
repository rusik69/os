/* init.c — system initialization (not PID 1, so harmless) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("init: already running\n");
    return 1;
}
