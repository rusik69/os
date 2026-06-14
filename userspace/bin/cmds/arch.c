/* arch.c — print machine architecture using uname */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    struct utsname buf;
    if (uname(&buf) < 0) {
        printf("arch: uname failed\n");
        return 1;
    }
    printf("%s\n", buf.machine);
    return 0;
}
