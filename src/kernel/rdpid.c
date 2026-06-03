/* rdpid.c — RDPID instruction support (Read Processor ID) */

#include "cpu_features.h"
#include "cpu.h"
#include "printf.h"

static int rdpid_available = 0;

int rdpid_init(void) {
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 7, subleaf 0 for RDPID (EBX bit 22) */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(7), "c"(0));

    if (!(rbx & CPUID_7_EBX_RDPID)) {
        kprintf("[cpu] RDPID instruction not supported\n");
        return -1;
    }

    rdpid_available = 1;
    kprintf("[cpu] RDPID instruction available\n");
    return 0;
}

int rdpid_is_available(void) {
    return rdpid_available;
}
