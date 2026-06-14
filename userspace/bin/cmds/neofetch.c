/* neofetch.c — system info display */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void) {
    struct utsname uts;
    if (uname(&uts) >= 0) {
        printf("OS: %s %s %s\n", uts.sysname, uts.release, uts.machine);
    }
    /* Try reading /proc/cpuinfo */
    int fd = open("/proc/cpuinfo", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[512];
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            char *p = strstr(buf, "model name");
            if (p) {
                char *colon = strchr(p, ':');
                if (colon) {
                    colon++;
                    while (*colon == ' ') colon++;
                    char *nl = strchr(colon, '\n');
                    if (nl) *nl = 0;
                    printf("CPU: %s\n", colon);
                }
            }
        }
        close(fd);
    }
    /* Read /proc/meminfo */
    fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[512];
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            char *p = strstr(buf, "MemTotal");
            if (p) {
                char *colon = strchr(p, ':');
                if (colon) {
                    colon++;
                    while (*colon == ' ') colon++;
                    char *nl = strchr(colon, '\n');
                    if (nl) *nl = 0;
                    printf("Memory: %s\n", colon);
                }
            }
        }
        close(fd);
    }
    /* Uptime */
    fd = open("/proc/uptime", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[64];
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            char *space = strchr(buf, ' ');
            if (space) *space = 0;
            unsigned long sec = 0;
            for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++)
                sec = sec * 10 + (unsigned long)(buf[i] - '0');
            printf("Uptime: %lu sec\n", sec);
        }
        close(fd);
    }
    return 0;
}
