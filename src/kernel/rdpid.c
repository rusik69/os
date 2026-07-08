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
        kprintf("[CPU] RDPID instruction not supported\n");
        return -1;
    }

    rdpid_available = 1;
    kprintf("[CPU] RDPID instruction available\n");
    return 0;
}

static int rdpid_is_available(void) {
    return rdpid_available;
}

/* ── Stub: rdpid_read ─────────────────────────────── */
static uint64_t rdpid_read(void)
{
    kprintf("[RDPID] rdpid_read: not yet implemented\n");
    return 0;
}
/* ── Stub: rdpid_write ─────────────────────────────── */
static int rdpid_write(uint64_t val)
{
    (void)val;
    kprintf("[RDPID] rdpid_write: not yet implemented\n");
    return 0;
}
