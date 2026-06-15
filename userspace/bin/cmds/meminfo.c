/* meminfo.c — Show memory info using sysinfo() */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void) {
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        /* Fallback: try /proc/meminfo */
        int fd = open("/proc/meminfo", 0, 0);
        if (fd >= 0) {
            char buf[4096];
            int n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                write(1, buf, n);
            close(fd);
            return 0;
        }
        printf("meminfo: sysinfo failed and /proc/meminfo not available\n");
        return 1;
    }

    unsigned long long unit = info.mem_unit ? (unsigned long long)info.mem_unit : 1ULL;
    unsigned long long total_kb = info.totalram * unit / 1024;
    unsigned long long free_kb = info.freeram * unit / 1024;
    unsigned long long used_kb = (info.totalram - info.freeram) * unit / 1024;
    unsigned long long buffers_kb = info.bufferram * unit / 1024;
    unsigned long long shared_kb = info.sharedram * unit / 1024;
    unsigned long long swap_total_kb = info.totalswap * unit / 1024;
    unsigned long long swap_free_kb = info.freeswap * unit / 1024;
    unsigned long long swap_used_kb = (info.totalswap - info.freeswap) * unit / 1024;

    printf("              total        used        free      shared     buffers\n");
    printf("Mem:    %11llu %11llu %11llu %11llu %11llu\n",
           total_kb, used_kb, free_kb, shared_kb, buffers_kb);
    printf("-/+ buffers:      %11llu %11llu\n",
           used_kb - buffers_kb, free_kb + buffers_kb);
    printf("Swap:   %11llu %11llu %11llu\n",
           swap_total_kb, swap_used_kb, swap_free_kb);

    return 0;
}
