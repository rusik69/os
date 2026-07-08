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
        kprintf("[CPU] FSGSBASE instructions not supported\n");
        return -1;
    }

    /* Enable FSGSBASE in CR4 (bit 16) */
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_FSGSBASE;
    write_cr4(cr4);

    fsgsbase_available = 1;
    kprintf("[CPU] FSGSBASE instructions enabled (CR4 bit 16)\n");
    return 0;
}

static int fsgsbase_is_available(void) {
    return fsgsbase_available;
}

/* ── Stub: fsgsbase_read_gs ─────────────────────────────── */
static uint64_t fsgsbase_read_gs(void)
{
    kprintf("[FSGSBASE] fsgsbase_read_gs: not yet implemented\n");
    return 0;
}
/* ── Stub: fsgsbase_write_gs ─────────────────────────────── */
static int fsgsbase_write_gs(uint64_t val)
{
    (void)val;
    kprintf("[FSGSBASE] fsgsbase_write_gs: not yet implemented\n");
    return 0;
}
/* ── Stub: fsgsbase_read_fs ─────────────────────────────── */
static uint64_t fsgsbase_read_fs(void)
{
    kprintf("[FSGSBASE] fsgsbase_read_fs: not yet implemented\n");
    return 0;
}
/* ── Stub: fsgsbase_write_fs ─────────────────────────────── */
static int fsgsbase_write_fs(uint64_t val)
{
    (void)val;
    kprintf("[FSGSBASE] fsgsbase_write_fs: not yet implemented\n");
    return 0;
}
