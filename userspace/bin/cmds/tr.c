#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 3) { printf("Usage: tr <set1> <set2>\n"); return 1; }
    const char *set1 = argv[1];
    const char *set2 = argv[2];
    char buf[4096];
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            const char *p = strchr(set1, buf[i]);
            if (p && (p - set1) < (int)strlen(set2)) buf[i] = set2[p - set1];
        }
        write(1, buf, n);
    }
    return 0;
}
