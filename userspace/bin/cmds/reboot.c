#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(void) {
    printf("Rebooting...\n");
    reboot();
    /* should not return */
    printf("reboot: failed\n");
    return 1;
}
