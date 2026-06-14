#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static unsigned long my_atoul(const char *s) {
    unsigned long r = 0;
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    return r;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: factor <number> [number...]\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        unsigned long n = my_atoul(argv[i]);
        printf("%lu:", n);
        if (n < 2) { printf(" %lu\n", n); continue; }
        unsigned long m = n;
        for (unsigned long f = 2; f * f <= m; f++) {
            while (m % f == 0) { printf(" %lu", f); m /= f; }
        }
        if (m > 1) printf(" %lu", m);
        printf("\n");
    }
    return 0;
}
