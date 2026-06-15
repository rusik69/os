/* cpuinfo.c — Show CPU info */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Try /proc/cpuinfo first */
    int fd = open("/proc/cpuinfo", 0, 0);
    if (fd >= 0) {
        char buf[4096];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, n);
        }
        close(fd);
        return 0;
    }

    /* Fallback: use sysinfo and uname */
    struct utsname uts;
    struct sysinfo si;
    int have_uname = (uname(&uts) == 0);
    int have_sys = (sysinfo(&si) == 0);

    printf("CPU Information:\n");
    if (have_uname) {
        printf("  Machine:           %s\n", uts.machine);
        printf("  System:            %s %s %s\n", uts.sysname, uts.release, uts.version);
        printf("  Hostname:          %s\n", uts.nodename);
    }
    if (have_sys) {
        printf("  Processors:        %u\n", (unsigned int)si.procs);
    }
    printf("  Features:          (kernel provides /proc/cpuinfo for details)\n");

    return 0;
}
