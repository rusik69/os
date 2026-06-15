/* ipsec.c — IPSec configuration */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("IPSec is configured via kernel shell 'ipsec' command.\n");
    return 0;
}
