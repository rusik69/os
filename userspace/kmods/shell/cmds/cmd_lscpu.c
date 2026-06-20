/* cmd_lscpu.c — Show CPU info */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_lscpu(const char *args) {
    (void)args;
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];

    /* CPUID leaf 0: vendor */
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)&vendor[0] = ebx;
    *(uint32_t*)&vendor[4] = edx;
    *(uint32_t*)&vendor[8] = ecx;
    vendor[12] = '\0';

    /* CPUID leaf 1: features + topology */
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    uint8_t stepping = (uint8_t)(eax & 0xF);
    uint8_t model    = (uint8_t)((eax >> 4) & 0xF);
    uint8_t family   = (uint8_t)((eax >> 8) & 0xF);
    uint8_t logical  = (uint8_t)((ebx >> 16) & 0xFF);
    uint8_t clflush  = (uint8_t)((ebx >> 8) & 0xFF);

    /* Extended family/model if needed */
    uint8_t ext_model = (uint8_t)((eax >> 16) & 0xF);
    uint8_t ext_family = (uint8_t)((eax >> 20) & 0xFF);
    if (family == 0xF) family += ext_family;
    if (family == 0xF || (family == 0x6))
        model = (uint8_t)((ext_model << 4) | model);

    /* Max extended leaf */
    uint32_t max_ext;
    __asm__ volatile("cpuid" : "=a"(max_ext) : "a"(0x80000000) : "ebx","ecx","edx");

    /* Brand string */
    char brand[49];
    memset(brand, 0, 49);
    if (max_ext >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000002 + i));
            *(uint32_t*)&brand[i*16+0] = eax;
            *(uint32_t*)&brand[i*16+4] = ebx;
            *(uint32_t*)&brand[i*16+8] = ecx;
            *(uint32_t*)&brand[i*16+12] = edx;
        }
    }

    /* L1 cache from extended leaf */
    uint32_t l1_size = 0;
    if (max_ext >= 0x80000005) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000005));
        l1_size = (ecx >> 24) & 0xFF;
    }

    kprintf("Architecture:        x86_64\n");
    kprintf("CPU op-mode(s):      64-bit\n");
    kprintf("Vendor ID:           %s\n", vendor);
    if (brand[0]) kprintf("Model name:          %s\n", brand);
    kprintf("CPU family:          %u\n", (unsigned int)family);
    kprintf("Model:               %u\n", (unsigned int)model);
    kprintf("Stepping:            %u\n", (unsigned int)stepping);
    kprintf("CPU(s):              %u\n", (unsigned int)logical);
    kprintf("Thread(s) per core:  %u\n", (unsigned int)logical);
    kprintf("CLFLUSH line size:   %u\n", (unsigned int)clflush);
    kprintf("L1d cache:           %uK\n", (unsigned int)l1_size);
}
