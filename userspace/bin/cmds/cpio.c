/* cpio.c — copy in/out (stub: just copy files) */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: cpio -o < name-list [archive]\\n");
        printf("       cpio -i [archive]\\n");
        printf("       cpio -p < dest\\n");
        return 1;
    }
    (void)argv;
    printf("cpio: not fully implemented (use cp instead)\n");
    return 1;
}
