/* x2apic.c — x2APIC detection and enable */

#include "cpu_features.h"
#include "cpu.h"
#include "printf.h"
#include "apic.h"

static int x2apic_active = 0;

int x2apic_init(void) {
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for x2APIC support (ECX bit 21) */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    if (!(rcx & (1 << 21))) {
        kprintf("[cpu] x2APIC not supported\n");
        return -1;
    }

    kprintf("[cpu] x2APIC supported by CPU\n");

    /* Read current APIC base MSR */
    uint64_t apic_base = read_msr(IA32_APIC_BASE);

    /* Check if already in x2APIC mode */
    if (apic_base & IA32_APIC_BASE_X2APIC) {
        kprintf("[cpu] x2APIC already active\n");
        x2apic_active = 1;
        return 0;
    }

    /* Switch to x2APIC mode:
     * 1. Software-disable the local APIC (clear bit 11)
     * 2. Set the x2APIC enable bit (bit 10)
     * 3. Re-enable the local APIC (set bit 11) */
    apic_base &= ~IA32_APIC_BASE_ENABLE;  /* Disable APIC first */
    write_msr(IA32_APIC_BASE, apic_base);

    apic_base |= IA32_APIC_BASE_X2APIC;   /* Enable x2APIC */
    apic_base |= IA32_APIC_BASE_ENABLE;   /* Re-enable APIC */
    write_msr(IA32_APIC_BASE, apic_base);

    /* Verify the transition */
    uint64_t check = read_msr(IA32_APIC_BASE);
    if (check & IA32_APIC_BASE_X2APIC) {
        kprintf("[cpu] x2APIC enabled successfully\n");
        x2apic_active = 1;
        return 0;
    } else {
        kprintf("[cpu] x2APIC enable FAILED\n");
        return -1;
    }
}

int x2apic_is_active(void) {
    return x2apic_active;
}
