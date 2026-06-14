/* df.c — report filesystem disk space usage via statfs */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : "/";
    struct statfs st;
    if (statfs(path, &st) < 0) {
        printf("df: cannot stat '%s'\n", path);
        return 1;
    }
    unsigned long long total = st.f_blocks * st.f_bsize;
    unsigned long long free  = st.f_bfree * st.f_bsize;
    unsigned long long used  = total - free;
    printf("Filesystem    1K-blocks     Used    Available Use%% Mounted on\n");
    printf("%-14s %-10llu %-7llu %-9llu %llu%% %s\n",
           "rootfs",
           total / 1024, used / 1024, free / 1024,
           total > 0 ? used * 100 / total : 0,
           path);
    return 0;
}
