#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define BUF_SIZE 4096

static char *read_file(const char *path, long *size) {
    int fd = open(path, 0, 0);
    if (fd < 0) return NULL;
    char *data = malloc(BUF_SIZE);
    if (!data) { if (fd != 0) close(fd); return NULL; }
    long cap = BUF_SIZE;
    long len = 0;
    int n;
    while ((n = read(fd, data + len, cap - len)) > 0) {
        len += n;
        if (cap - len < 1024) {
            cap *= 2;
            char *nd = realloc(data, cap);
            if (!nd) { close(fd); free(data); return NULL; }
            data = nd;
        }
    }
    close(fd);
    data[len] = '\0';
    *size = len;
    return data;
}

int main(int argc, char *argv[]) {
    if (argc < 3) { printf("Usage: diff <file1> <file2>\n"); return 1; }
    long s1, s2;
    char *f1 = read_file(argv[1], &s1);
    char *f2 = read_file(argv[2], &s2);
    if (!f1 || !f2) { printf("diff: cannot open files\n"); free(f1); free(f2); return 1; }
    char *l1[8192], *l2[8192];
    int nl1 = 0, nl2 = 0;
    l1[nl1++] = f1;
    for (char *p = f1; *p; p++) if (*p == '\n') { *p = '\0'; if (*(p+1)) l1[nl1++] = p+1; }
    l2[nl2++] = f2;
    for (char *p = f2; *p; p++) if (*p == '\n') { *p = '\0'; if (*(p+1)) l2[nl2++] = p+1; }
    int diff = 0;
    int max = nl1 > nl2 ? nl1 : nl2;
    for (int i = 0; i < max; i++) {
        if (i >= nl1) {
            printf("%da%d\n> %s\n", i, i+1, l2[i]);
            diff = 1;
        } else if (i >= nl2) {
            printf("%dd%d\n< %s\n", i+1, i, l1[i]);
            diff = 1;
        } else if (strcmp(l1[i], l2[i]) != 0) {
            printf("%dc%d\n< %s\n---\n> %s\n", i+1, i+1, l1[i], l2[i]);
            diff = 1;
        }
    }
    free(f1);
    free(f2);
    return diff;
}
