#include "cpu.h"
#include "printf.h"

/* Enable CPU security features: SMEP, SMAP, NXE, UMIP.
 *
 * SMEP (CR4 bit 20): Prevents ring-0 execution of user-space pages.
 *   Without this, a kernel bug that jumps to a user-controlled address
 *   would execute in ring 0, bypassing all isolation.
 *
 * SMAP (CR4 bit 21): Prevents ring-0 access to user-space data.
 *   Kernel must use stac/clac instructions to temporarily enable
 *   user data access. Catches bugs where syscall handlers forget
 *   to validate user pointers.
 *
 * NXE  (EFER bit 11): Enables the No-Execute page table bit (bit 63).
 *   Allows marking data pages as non-executable. User stack and
 *   data pages should be marked NX to prevent code injection.
 *
 * UMIP (CR4 bit 11): Prevents user-mode execution of SGDT, SIDT,
 *   SLDT, SMSW, STR instructions. These leak kernel addresses.
 *
 * Returns 0 on success, -1 if any feature is unavailable. */
int cpu_security_init(void) {
    uint64_t cr4 = read_cr4();
    uint64_t efer = read_msr(0xC0000080); /* IA32_EFER */
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for feature bits */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    /* Enable SMEP if supported (ECX bit 7) */
    if (rcx & (1 << 7)) {
        cr4 |= CR4_SMEP;
        kprintf("[cpu] SMEP enabled\n");
    } else {
        kprintf("[cpu] SMEP not supported\n");
    }

    /* Enable SMAP if supported (ECX bit 20) */
    if (rcx & (1 << 20)) {
        cr4 |= CR4_SMAP;
        kprintf("[cpu] SMAP enabled\n");
    } else {
        kprintf("[cpu] SMAP not supported\n");
    }

    /* Enable UMIP if supported (ECX bit 2) */
    if (rcx & (1 << 2)) {
        cr4 |= CR4_UMIP;
        kprintf("[cpu] UMIP enabled\n");
    } else {
        kprintf("[cpu] UMIP not supported\n");
    }

    /* Write updated CR4 */
    write_cr4(cr4);

    /* Enable NXE in EFER if supported */
    if (rdx & (1 << 20)) { /* CPUID.1:EDX bit 20 = NX (XD) support */
        efer |= EFER_NXE;
        write_msr(0xC0000080, efer);
        kprintf("[cpu] NXE enabled\n");
    } else {
        kprintf("[cpu] NX not supported\n");
    }

    return 0;
}
