/* doom.c — DOOM is a kernel-level service */
#include "unistd.h"
#include "stdio.h"

int main(void) {
    printf("doom is a kernel task -- run 'doom' in the kernel shell, "
           "or run 'doom_task' as a kernel service. "
           "Userspace client not available.\n");
    return 0;
}
