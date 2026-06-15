/* lslocks.c — list file locks stub */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("File locks are tracked by the kernel. "
           "Use 'lslocks' from the kernel shell.\n");
    return 0;
}
