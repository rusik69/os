/* smap_smep_umip.c — SMAP, SMEP, and UMIP CR4 enable/init */

#include "cpu_features.h"
#include "cpu.h"
#include "printf.h"

int smap_smep_init(void) {
    uint64_t cr4 = read_cr4();
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for SMEP (ECX bit 7) */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    /* Check CPUID leaf 7, subleaf 0 for SMAP (EBX bit 20) — leaf 1 ECX
     * bit 20 is SSE4.2, NOT SMAP; probing the wrong leaf would set
     * CR4.SMAP on CPUs without SMAP, causing a #GP on the CR4 write. */
    int l7_ebx, l7_ecx;
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(l7_ebx), "=c"(l7_ecx), "=d"(rdx) : "a"(7), "c"(0));

    /* Enable SMEP if supported (leaf 1 ECX bit 7) */
    if (rcx & (1U << 7)) {
        cr4 |= CR4_SMEP;
        kprintf("[CPU] SMEP enabled (CR4 bit 20)\n");
    } else {
        kprintf("[CPU] SMEP not supported\n");
    }

    /* Enable SMAP if supported (leaf 7 EBX bit 20) */
    if (l7_ebx & (1U << 20)) {
        cr4 |= CR4_SMAP;
        kprintf("[CPU] SMAP enabled (CR4 bit 21)\n");
    } else {
        kprintf("[CPU] SMAP not supported\n");
    }

    /* Write updated CR4 */
    write_cr4(cr4);
    return 0;
}

int umip_init(void) {
    uint64_t cr4 = read_cr4();
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 7, subleaf 0 for UMIP (ECX bit 2) — leaf 1 ECX
     * bit 2 is DTES64, NOT UMIP; probing the wrong leaf would set
     * CR4.UMIP on CPUs without UMIP, causing a #GP on the CR4 write. */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(7), "c"(0));

    /* Enable UMIP if supported (leaf 7 ECX bit 2) */
    if (rcx & (1U << 2)) {
        cr4 |= CR4_UMIP;
        write_cr4(cr4);
        kprintf("[CPU] UMIP enabled (CR4 bit 11)\n");
    } else {
        kprintf("[CPU] UMIP not supported\n");
    }
    return 0;
}
