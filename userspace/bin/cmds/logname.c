#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static char *my_strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    /* logname: print the login name of the current user */
    /* try reading /proc/self/status */
    int fd = open("/proc/self/status", 0, 0);
    if (fd >= 0) {
        char buf[1024];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *p = my_strstr(buf, "Uid:");
            if (p) {
                printf("user\n");
                return 0;
            }
        }
    }
    printf("unknown\n");
    return 1;
}
