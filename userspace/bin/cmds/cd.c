/* cd.c — print directory (cd is a shell builtin) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: cd <directory>\n");
        return 1;
    }
    if (chdir(argv[1]) < 0) {
        printf("cd: %s: No such directory\n", argv[1]);
        return 1;
    }
    char buf[512];
    if (getcwd(buf, sizeof(buf)) >= 0)
        printf("%s\n", buf);
    return 0;
}
