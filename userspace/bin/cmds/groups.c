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
    /* groups: print group memberships of the current user */
    /* try reading /proc/self/status for group info */
    int fd = open("/proc/self/status", 0, 0);
    if (fd >= 0) {
        char buf[4096];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        buf[n] = '\0';
        char *p = my_strstr(buf, "Gid:");
        if (p) {
            char gidline[128];
            int i = 0;
            p += 4;
            while (*p && *p != '\n' && i < 127) gidline[i++] = *p++;
            gidline[i] = '\0';
            printf("%s : %s\n", "user", gidline);
            return 0;
        }
    }
    printf("user : unknown\n");
    return 1;
}
