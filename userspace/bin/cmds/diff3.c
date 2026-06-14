/* diff3.c — three-way diff (stub) */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("usage: diff3 <mine> <older> <yours>\n");
        return 1;
    }
    (void)argv;
    printf("diff3: not supported (use diff instead)\n");
    return 1;
}
