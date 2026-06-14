/* ipcrm.c — remove IPC resources (stub) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("usage: ipcrm [shm|msg|sem] <id>...\n");
        return 1;
    }
    printf("ipcrm: not supported\n");
    return 1;
}
