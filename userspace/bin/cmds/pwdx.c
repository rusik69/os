/* pwdx.c — Print working directory of process (stub) */
#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: pwdx <pid>\n");
        return 1;
    }
    int pid = atoi(argv[1]);
    if (pid <= 0) {
        printf("pwdx: invalid pid\n");
        return 1;
    }
    /* Try to read /proc/<pid>/cwd symlink or just stub */
    printf("%d: /\n", pid);
    return 0;
}
