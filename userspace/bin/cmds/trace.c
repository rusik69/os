/* trace.c — system call tracing is a kernel-level service */
#include "unistd.h"
#include "stdio.h"

int main(void) {
    printf("Tracing is available in the kernel shell as 'trace' command. "
           "Use kernel shell for system call tracing.\n");
    return 0;
}
