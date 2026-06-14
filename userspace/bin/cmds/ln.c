/* ln.c — create links (stub: print what would be done) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int sym = 0;
    int optind = 1;
    if (argc > 1 && strcmp(argv[1], "-s") == 0) { sym = 1; optind = 2; }
    if (argc - optind < 2) { printf("Usage: ln [-s] <target> <linkname>\n"); return 1; }
    const char *target = argv[optind];
    const char *linkname = argv[optind + 1];
    if (sym)
        printf("ln: would create symlink '%s' -> '%s'\n", linkname, target);
    else
        printf("ln: would create hard link '%s' -> '%s'\n", linkname, target);
    return 0;
}
