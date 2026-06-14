/* loadpin.c — Loadpin status stub */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    if (argc >= 2 && strcmp(argv[1], "-v") == 0) {
        printf("loadpin: not enforced (stub)\n");
        return 0;
    }
    printf("loadpin disabled\n");
    return 0;
}
