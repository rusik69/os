#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int do_halt = 0;
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--halt") == 0)
            do_halt = 1;
        else if (strcmp(argv[1], "-r") == 0 || strcmp(argv[1], "--reboot") == 0)
            do_halt = 0;
        else if (strcmp(argv[1], "--help") == 0) {
            printf("Usage: shutdown [-h|-r]\n");
            return 0;
        }
    }
    if (do_halt) {
        printf("Halting system...\n");
    } else {
        printf("Rebooting system...\n");
    }
    printf("shutdown: not fully implemented\n");
    return 1;
}
