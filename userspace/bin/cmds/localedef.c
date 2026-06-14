/* localedef.c — define locale (stub) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("usage: localedef <locale-name>\n");
        return 1;
    }
    printf("localedef: not yet implemented\n");
    return 1;
}
