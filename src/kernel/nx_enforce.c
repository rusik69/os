/* nx_enforce.c — NX-bit enforcement for user pages */

#include "cpu.h"
#include "cpu_features.h"
#include "printf.h"

static int nx_enforce_active = 0;

int nx_enforce_init(void) {
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for NX support (EDX bit 20) */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    if (!(rdx & (1 << 20))) {
        kprintf("[cpu] NX (No-Execute) not supported\n");
        return;
    }

    /* Enable NXE in IA32_EFER MSR (bit 11) */
    uint64_t efer = read_msr(0xC0000080);
    efer |= EFER_NXE;
    write_msr(0xC0000080, efer);

    nx_enforce_active = 1;
    kprintf("[cpu] NX-bit enforcement enabled for user pages\n");
    return 0;
}

int nx_enforce_is_active(void) {
    return nx_enforce_active;
}
