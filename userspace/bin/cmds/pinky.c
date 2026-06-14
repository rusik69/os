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
    /* pinky: simplified finger-like user info */
    const char *user = argc > 1 ? argv[1] : NULL;
    /* try reading /proc/self/status for user info */
    int fd = open("/proc/self/status", 0, 0);
    if (fd >= 0) {
        char buf[4096];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        buf[n] = '\0';
        char name[64] = "unknown";
        char uid[16] = "?";
        char gid[16] = "?";
        char *p;
        p = my_strstr(buf, "Name:");
        if (p) { p += 5; while (*p == ' ' || *p == '\t') p++; char *e = p; while (*e && *e != '\n') e++; int l = e-p; if (l>63)l=63; memcpy(name, p, l); name[l]='\0'; }
        p = my_strstr(buf, "Uid:");
        if (p) { p += 4; while (*p == ' ' || *p == '\t') p++; char *e = p; while (*e && *e != '\n') e++; int l = e-p; if (l>15)l=15; memcpy(uid, p, l); uid[l]='\0'; }
        p = my_strstr(buf, "Gid:");
        if (p) { p += 4; while (*p == ' ' || *p == '\t') p++; char *e = p; while (*e && *e != '\n') e++; int l = e-p; if (l>15)l=15; memcpy(gid, p, l); gid[l]='\0'; }
        printf("Login: %s\t\t\tName: %s\n", name, name);
        printf("Uid: %s\t\t\tGid: %s\n", uid, gid);
        if (user) printf("Real user: %s\n", user);
        return 0;
    }
    if (user) printf("Login: %s\n", user);
    else printf("Login: unknown\n");
    return 0;
}
