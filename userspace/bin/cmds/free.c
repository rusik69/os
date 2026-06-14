/* free.c — display memory usage via sysinfo */

#include "unistd.h"
#include "stdio.h"

int main(void) {
    struct sysinfo info;
    if (sysinfo(&info) < 0) {
        printf("free: sysinfo failed\n");
        return 1;
    }
    unsigned long long total = info.totalram * info.mem_unit;
    unsigned long long free  = info.freeram * info.mem_unit;
    unsigned long long used  = total - free;
    unsigned long long shared = info.sharedram * info.mem_unit;
    unsigned long long buf   = info.bufferram * info.mem_unit;
    printf("              total        used        free      shared     buffers\n");
    printf("Mem:     %10llu %10llu %10llu %10llu %10llu\n",
           total / 1024, used / 1024, free / 1024, shared / 1024, buf / 1024);
    return 0;
}
