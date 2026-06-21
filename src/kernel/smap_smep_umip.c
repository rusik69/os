/* smap_smep_umip.c — SMAP, SMEP, and UMIP CR4 enable/init */

#include "cpu_features.h"
#include "cpu.h"
#include "printf.h"

int smap_smep_init(void) {
    uint64_t cr4 = read_cr4();
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for feature bits */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    /* Enable SMEP if supported (ECX bit 7) */
    if (rcx & (1 << 7)) {
        cr4 |= CR4_SMEP;
        kprintf("[cpu] SMEP enabled (CR4 bit 20)\n");
    } else {
        kprintf("[cpu] SMEP not supported\n");
    }

    /* Enable SMAP if supported (ECX bit 20) */
    if (rcx & (1 << 20)) {
        cr4 |= CR4_SMAP;
        kprintf("[cpu] SMAP enabled (CR4 bit 21)\n");
    } else {
        kprintf("[cpu] SMAP not supported\n");
    }

    /* Write updated CR4 */
    write_cr4(cr4);
    return 0;
}

int umip_init(void) {
    uint64_t cr4 = read_cr4();
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for feature bits */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    /* Enable UMIP if supported (ECX bit 2) */
    if (rcx & (1 << 2)) {
        cr4 |= CR4_UMIP;
        write_cr4(cr4);
        kprintf("[cpu] UMIP enabled (CR4 bit 11)\n");
    } else {
        kprintf("[cpu] UMIP not supported\n");
    }
    return 0;
}

/* ── Stub: smap_enable ─────────────────────────────── */
int smap_enable(void)
{
    kprintf("[smap] smap_enable: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: smap_disable ─────────────────────────────── */
int smap_disable(void)
{
    kprintf("[smap] smap_disable: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: smep_enable ─────────────────────────────── */
int smep_enable(void)
{
    kprintf("[smap] smep_enable: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: smep_disable ─────────────────────────────── */
int smep_disable(void)
{
    kprintf("[smap] smep_disable: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: umip_handle_insn ─────────────────────────────── */
int umip_handle_insn(void *regs)
{
    (void)regs;
    kprintf("[smap] umip_handle_insn: not yet implemented\n");
    return -ENOSYS;
}
