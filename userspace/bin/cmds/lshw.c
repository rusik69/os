/* lshw.c — hardware configuration */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void){
    printf("hermes-os\n");
    printf("    description: Computer\n");
    printf("    product: QEMU Standard PC\n");
    printf("  *-core\n");
    printf("       description: Motherboard\n");
    printf("       physical id: 0\n");
    printf("     *-cpu\n");
    printf("          description: CPU\n");
    printf("          product: QEMU Virtual CPU\n");
    printf("          physical id: 0\n");
    printf("          size: 2394MHz\n");
    printf("          capacity: 2394MHz\n");
    printf("     *-memory\n");
    printf("          description: System Memory\n");
    printf("          physical id: 1\n");
    printf("          size: 256MiB\n");
    printf("     *-pci\n");
    printf("          description: PCI bus\n");
    printf("          physical id: 2\n");
    return 0;
}
