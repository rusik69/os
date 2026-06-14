#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("Usage: ftpput LOCAL_FILE REMOTE_FILE\n");
        return 1;
    }
    printf("ftpput: not implemented\n");
    return 1;
}
