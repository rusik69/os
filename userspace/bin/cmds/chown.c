#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 3) {
        printf("Usage: chown OWNER[:GROUP] FILE...\n");
        return 1;
    }
    printf("chown: not implemented\n");
    return 1;
}
