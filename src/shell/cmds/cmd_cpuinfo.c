/* cmd_cpuinfo.c — cpuinfo command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_cpuinfo(void) {
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)&vendor[0] = ebx;
    *(uint32_t*)&vendor[4] = edx;
    *(uint32_t*)&vendor[8] = ecx;
    vendor[12] = 0;
    kprintf("CPU Vendor: %s\n", vendor);

    char brand[49];
    memset(brand, 0, 49);
    for (uint32_t i = 0; i < 3; i++) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000002 + i));
        *(uint32_t*)&brand[i*16+0] = eax;
        *(uint32_t*)&brand[i*16+4] = ebx;
        *(uint32_t*)&brand[i*16+8] = ecx;
        *(uint32_t*)&brand[i*16+12] = edx;
    }
    kprintf("CPU Brand:  %s\n", brand);
}
