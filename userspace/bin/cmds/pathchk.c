/* pathchk.c — check path validity */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: pathchk PATH...\n");
        return 1;
    }
    int all_ok = 0;
    for (int i = 1; i < argc; i++) {
        char *p = argv[i];
        if (!p || *p == '\0') {
            printf("pathchk: '%s': empty path\n", argv[i]);
            all_ok = 1;
            continue;
        }
        unsigned long len = strlen(p);
        if (len > 4096) {
            printf("pathchk: '%s': path too long (%lu > 4096)\n", argv[i], len);
            all_ok = 1;
            continue;
        }
        /* Check for null in path */
        int has_null = 0;
        for (unsigned long j = 0; j < len; j++) {
            if (p[j] == '\0') { has_null = 1; break; }
        }
        if (has_null) {
            printf("pathchk: '%s': contains null byte\n", argv[i]);
            all_ok = 1;
            continue;
        }
        printf("pathchk: '%s': valid\n", argv[i]);
    }
    return all_ok ? 1 : 0;
}
