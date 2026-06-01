/* fsgsbase.c — FSGSBASE instructions (RDFSBASE, WRFSBASE, RDGSBASE, WRGSBASE) */

#include "cpu_features.h"
#include "cpu.h"
#include "printf.h"

static int fsgsbase_available = 0;

int fsgsbase_init(void) {
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 7, subleaf 0 for FSGSBASE (EBX bit 0) */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(7), "c"(0));

    if (!(rbx & CPUID_7_EBX_FSGSBASE)) {
        kprintf("[cpu] FSGSBASE instructions not supported\n");
        return;
    }

    /* Enable FSGSBASE in CR4 (bit 16) */
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_FSGSBASE;
    write_cr4(cr4);

    fsgsbase_available = 1;
    kprintf("[cpu] FSGSBASE instructions enabled (CR4 bit 16)\n");
    return 0;
}

int fsgsbase_is_available(void) {
    return fsgsbase_available;
}
