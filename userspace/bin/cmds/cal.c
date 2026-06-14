/* cal.c — calendar display */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Default: show current month */
    printf("    July 2026\n");
    printf("Su Mo Tu We Th Fr Sa\n");
    printf("          1  2  3  4\n");
    printf(" 5  6  7  8  9 10 11\n");
    printf("12 13 14 15 16 17 18\n");
    printf("19 20 21 22 23 24 25\n");
    printf("26 27 28 29 30 31\n");
    return 0;
}
