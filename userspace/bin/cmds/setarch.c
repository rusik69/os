/* setarch.c — print/set architecture */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Just report the architecture */
    printf("x86_64\n");
    return 0;
}
