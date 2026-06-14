/* id.c — print user and group IDs */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int uid = getuid();
    int euid = geteuid();
    int gid = getgid();
    int egid = getegid();
    printf("uid=%d euid=%d gid=%d egid=%d\n", uid, euid, gid, egid);
    return 0;
}
