/* serial.c — Serial terminal stub */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        printf("serial: COM1 at 0x3F8, IRQ 4\n");
        return 0;
    }
    printf("serial: not available\n");
    return 0;
}
