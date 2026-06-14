/* history.c — show command history (stub) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Try to read ~/.sh_history */
    int fd = open("/root/.sh_history", 0, 0);
    if (fd >= 0) {
        char buf[4096];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, n);
        }
        close(fd);
        return 0;
    }
    printf("history: no command history found\n");
    return 0;
}
