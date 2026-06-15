/* sshd.c — SSH daemon: use kernel shell command */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(void) {
    printf("sshd: Use the kernel shell's sshd command instead\n");
    return 0;
}
