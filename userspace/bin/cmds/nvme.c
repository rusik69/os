/* nvme.c — NVMe device info stub */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("NVMe is managed by the kernel's NVMe driver. "
           "Use 'nvme list' etc. from the kernel shell.\n");
    return 0;
}
