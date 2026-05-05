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

    /* CPUID leaf 1: feature flags */
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    uint8_t stepping  = eax & 0xF;
    uint8_t model     = (eax >> 4) & 0xF;
    uint8_t family    = (eax >> 8) & 0xF;
    uint8_t logical   = (ebx >> 16) & 0xFF;
    kprintf("Family: %u  Model: %u  Stepping: %u  LogicalCPUs: %u\n",
            (uint64_t)family, (uint64_t)model, (uint64_t)stepping, (uint64_t)logical);

    kprintf("Features:");
    if (edx & (1u<< 0)) kprintf(" FPU");
    if (edx & (1u<< 1)) kprintf(" VME");
    if (edx & (1u<< 4)) kprintf(" TSC");
    if (edx & (1u<< 5)) kprintf(" MSR");
    if (edx & (1u<< 6)) kprintf(" PAE");
    if (edx & (1u<< 9)) kprintf(" APIC");
    if (edx & (1u<<11)) kprintf(" SEP");
    if (edx & (1u<<15)) kprintf(" CMOV");
    if (edx & (1u<<19)) kprintf(" CLFL");
    if (edx & (1u<<23)) kprintf(" MMX");
    if (edx & (1u<<24)) kprintf(" FXSR");
    if (edx & (1u<<25)) kprintf(" SSE");
    if (edx & (1u<<26)) kprintf(" SSE2");
    if (edx & (1u<<28)) kprintf(" HTT");
    if (ecx & (1u<< 0)) kprintf(" SSE3");
    if (ecx & (1u<< 9)) kprintf(" SSSE3");
    if (ecx & (1u<<19)) kprintf(" SSE4.1");
    if (ecx & (1u<<20)) kprintf(" SSE4.2");
    if (ecx & (1u<<25)) kprintf(" AES");
    if (ecx & (1u<<26)) kprintf(" XSAVE");
    if (ecx & (1u<<28)) kprintf(" AVX");
    if (ecx & (1u<<30)) kprintf(" RDRAND");
    kprintf("\n");

    /* Extended features: long mode, etc. */
    uint32_t max_ext;
    __asm__ volatile("cpuid" : "=a"(max_ext) : "a"(0x80000000) : "ebx","ecx","edx");
    if (max_ext >= 0x80000001) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
        kprintf("ExtFeatures:");
        if (edx & (1u<<11)) kprintf(" SYSCALL");
        if (edx & (1u<<20)) kprintf(" NX");
        if (edx & (1u<<29)) kprintf(" LM(64-bit)");
        if (ecx & (1u<< 5)) kprintf(" ABM");
        if (ecx & (1u<< 6)) kprintf(" SSE4A");
        kprintf("\n");
    }
}
