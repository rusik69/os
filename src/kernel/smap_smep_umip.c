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
    if (rcx & (1U << 7)) {
        cr4 |= CR4_SMEP;
        kprintf("[CPU] SMEP enabled (CR4 bit 20)\n");
    } else {
        kprintf("[CPU] SMEP not supported\n");
    }

    /* Enable SMAP if supported (ECX bit 20) */
    if (rcx & (1U << 20)) {
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

    /* Check CPUID leaf 1 for feature bits */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    /* Enable UMIP if supported (ECX bit 2) */
    if (rcx & (1U << 2)) {
        cr4 |= CR4_UMIP;
        write_cr4(cr4);
        kprintf("[CPU] UMIP enabled (CR4 bit 11)\n");
    } else {
        kprintf("[CPU] UMIP not supported\n");
    }
    return 0;
}

/* ── smap_enable ─────────────────────────────── */
int smap_enable(void)
{
    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_SMAP)) {
        cr4 |= CR4_SMAP;
        write_cr4(cr4);
        kprintf("[SMAP] SMAP enabled\n");
    }
    return 0;
}

/* ── smap_disable ─────────────────────────────── */
int smap_disable(void)
{
    uint64_t cr4 = read_cr4();
    if (cr4 & CR4_SMAP) {
        cr4 &= ~CR4_SMAP;
        write_cr4(cr4);
        kprintf("[SMAP] SMAP disabled\n");
    }
    return 0;
}

/* ── smep_enable ─────────────────────────────── */
int smep_enable(void)
{
    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_SMEP)) {
        cr4 |= CR4_SMEP;
        write_cr4(cr4);
        kprintf("[SMAP] SMEP enabled\n");
    }
    return 0;
}

/* ── smep_disable ─────────────────────────────── */
int smep_disable(void)
{
    uint64_t cr4 = read_cr4();
    if (cr4 & CR4_SMEP) {
        cr4 &= ~CR4_SMEP;
        write_cr4(cr4);
        kprintf("[SMAP] SMEP disabled\n");
    }
    return 0;
}

/* ── umip_handle_insn ─────────────────────────────── */
int umip_handle_insn(void *regs)
{
    (void)regs;
    /* UMIP traps #GP on certain instructions (SGDT, SIDT, SLDT, SMSW, STR)
     * when executed from userspace. Return 0 to let the emulation happen
     * (if emulation is available) or -EPERM to kill the process. */
    kprintf("[SMAP] umip_handle_insn: #GP due to UMIP, instruction not emulated\n");
    return -EPERM;
}
