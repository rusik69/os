/* powercap.c — read/set power capping limits */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static void show_all(void) {
    /* Try Linux-standard /sys/class/powercap first */
    int fd = open("/sys/class/powercap", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            write(1, buf, n);
            return;
        }
    }

    /* Try /proc/powercap */
    fd = open("/proc/powercap", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            write(1, buf, n);
            return;
        }
    }

    printf("powercap: no powercap interface found\n");
    printf("powercap: available zones: package, core, uncore, dram\n");
    printf("powercap: use 'powercap set <watts>' to set limit\n");
}

static int set_limit(int watts) {
    if (watts <= 0) {
        printf("powercap: invalid power limit: %d W\n", watts);
        return 1;
    }

    /* Try /proc/powercap interface */
    int fd = open("/proc/powercap", O_WRONLY, 0);
    if (fd < 0) {
        printf("powercap: cannot set limit — no /proc/powercap interface\n");
        printf("powercap: use 'powercap set <watts>' from the kernel shell\n");
        return 1;
    }

    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%d\n", watts);
    write(fd, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf));
    close(fd);
    printf("powercap: limit set to %d W\n", watts);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        show_all();
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        return set_limit(atoi(argv[2]));
    }

    printf("usage: powercap                  (show all zones)\n");
    printf("       powercap set <watts>      (set power limit)\n");
    return 1;
}
