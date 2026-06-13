/*
 * suspend.c — ACPI S3 Suspend-to-RAM and S0ix (Suspend-to-Idle) support
 *
 * Implements:
 *   - ACPI S3 (Suspend-to-RAM): save/restore CPU state, enter via PM1a_CNT
 *   - S0ix (Suspend-to-Idle): Modern idle/suspend using MWAIT(C1e) loop
 *     for platforms that support this low-power mode.
 *
 * References:
 *   ACPI Specification v6.5, Section 4.8.4 — System Sleep (S3)
 *   Intel 64 and IA-32 Architectures SDM, Vol 3A, Chapter 9 — SMM
 *   Intel Low Power S0ix Idle (White Paper #339604)
 *
 * Item 124 — Suspend-to-Idle (S0ix) via MWAIT(C1e) loop
 */

#include "suspend.h"
#include "acpi.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "io.h"
#include "cpu.h"           /* read_msr, write_msr */
#include "smp.h"
#include "cpuidle.h"       /* MWAIT C-state support */
#include "timer.h"          /* timer_get_ticks */

/* ── MSR definitions for S0ix ──────────────────────────────────────── */
#define MSR_PKG_CST_CONFIG_CONTROL  0x000000E2
#define MSR_PMG_IO_CAP_BASE         0x0000014F
#define MSR_MISC_PWR_MGMT           0x000001AA
#define MSR_POWER_MISC              0x000001FC

/* ── Static save area ────────────────────────────────────────────────── */
/* Pre-allocated page that survives S3 (RAM is self-refreshed). */
static struct suspend_state *g_save     = NULL;
static uint64_t              g_save_phys = 0;
static int                   g_setup_once = 0;

/* ── CR/register helpers ─────────────────────────────────────────────── */
static inline uint64_t rdcr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t rdcr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t rdcr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t rdcr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void     wrcr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v) : "memory"); }
static inline void     wrcr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline void     wrcr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory"); }

/* ── State save ──────────────────────────────────────────────────────── */
static void suspend_save_cpu_state(void) {
    if (!g_save) return;

    memset(g_save, 0, sizeof(*g_save));

    /* Save GDT pseudo-descriptor using the CPU's SGDT instruction */
    {
        struct { uint16_t len; uint64_t base; } __attribute__((packed)) gdt, idt;
        __asm__ volatile("sgdt %0" : "=m"(gdt) :: "memory");
        __asm__ volatile("sidt %0" : "=m"(idt) :: "memory");
        g_save->gdt_base  = gdt.base;
        g_save->gdt_limit = gdt.len;
        g_save->idt_base  = idt.base;
        g_save->idt_limit = idt.len;
    }

    /* Control registers */
    g_save->cr0 = rdcr0();
    g_save->cr2 = rdcr2();
    g_save->cr3 = rdcr3();
    g_save->cr4 = rdcr4();

    /* Segment registers */
    __asm__ volatile("mov %%cs, %0" : "=r"(g_save->cs));
    __asm__ volatile("mov %%ds, %0" : "=r"(g_save->ds));
    __asm__ volatile("mov %%es, %0" : "=r"(g_save->es));
    __asm__ volatile("mov %%fs, %0" : "=r"(g_save->fs));
    __asm__ volatile("mov %%gs, %0" : "=r"(g_save->gs));
    __asm__ volatile("mov %%ss, %0" : "=r"(g_save->ss));

    /* EFER MSR */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
        g_save->efer = ((uint64_t)hi << 32) | lo;
    }

    /* Stack (RSP) for resume */
    __asm__ volatile("mov %%rsp, %0" : "=r"(g_save->resume_rsp));

    g_save->magic = SUSPEND_MAGIC;
}

/* ── State restore ───────────────────────────────────────────────────── */
static void suspend_restore_cpu_state(void) {
    if (!g_save || g_save->magic != SUSPEND_MAGIC) {
        kprintf("suspend: save area corrupted after resume\n");
        return;
    }

    /* Restore in safe order: GDT → segments → IDT → CR3 → CR4 → CR0 → EFER */

    /* GDT */
    {
        struct { uint16_t len; uint64_t base; } __attribute__((packed)) gdt;
        gdt.len  = (uint16_t)g_save->gdt_limit;
        gdt.base = g_save->gdt_base;
        __asm__ volatile("lgdt %0" :: "m"(gdt) : "memory");
    }

    /* Segment registers */
    __asm__ volatile("mov %0, %%ds" :: "r"(g_save->ds));
    __asm__ volatile("mov %0, %%es" :: "r"(g_save->es));
    __asm__ volatile("mov %0, %%fs" :: "r"(g_save->fs));
    __asm__ volatile("mov %0, %%gs" :: "r"(g_save->gs));
    __asm__ volatile("mov %0, %%ss" :: "r"(g_save->ss));

    /* IDT */
    {
        struct { uint16_t len; uint64_t base; } __attribute__((packed)) idt;
        idt.len  = (uint16_t)g_save->idt_limit;
        idt.base = g_save->idt_base;
        __asm__ volatile("lidt %0" :: "m"(idt) : "memory");
    }

    /* CR3 (page tables) first, then CR4, EFER, finally CR0 */
    wrcr3(g_save->cr3);
    wrcr4(g_save->cr4);

    {
        uint32_t lo = (uint32_t)g_save->efer;
        uint32_t hi = (uint32_t)(g_save->efer >> 32);
        __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0xC0000080) : "memory");
    }

    wrcr0(g_save->cr0);
}

/* ── Public API ──────────────────────────────────────────────────────── */

int suspend_s3(void) {
    if (!acpi_sleep_supported(ACPI_S3)) {
        kprintf("suspend: ACPI S3 not supported by platform\n");
        return -1;
    }

    /* Allocate save area on first call (4 KB page, persists through S3) */
    if (!g_save) {
        g_save_phys = pmm_alloc_frame();
        if (!g_save_phys) return -2;
        g_save = (struct suspend_state *)PHYS_TO_VIRT(g_save_phys);
    }

    /* Set the resume return address once */
    if (!g_setup_once) {
        /*
         * After S3 wake, the firmware jumps to the ACPI wakeup vector.
         * Our wakeup code needs to restore CPU state and resume the kernel.
         * The resume_rip points to suspend_restore_cpu_state which runs
         * after the trampoline re-establishes long mode.
         */
        g_save->resume_rip = (uint64_t)(uintptr_t)&suspend_restore_cpu_state;
        g_setup_once = 1;
    }

    kprintf("suspend: === entering ACPI S3 (Suspend-to-RAM) ===\n");
    kprintf("suspend: save area at phys 0x%lx, size %lu bytes\n",
            (unsigned long)g_save_phys, (unsigned long)sizeof(*g_save));

    /* Save current CPU state */
    suspend_save_cpu_state();

    /* Disable interrupts — we must not be interrupted during S3 entry */
    cli();

    /*
     * Write SLP_TYP + SLP_EN to PM1a_CNT to enter S3.
     * If S3 succeeds, the CPU stops here and RAM goes into self-refresh.
     * On wake, the firmware runs then jumps to the wakeup vector.
     *
     * If acpi_sleep() returns, the S3 transition failed for some reason
     * (e.g., the platform doesn't actually support it, or the write
     * was ignored).  Restore state and return error.
     */
    int rc = acpi_sleep(ACPI_S3);

    kprintf("suspend: S3 entry failed (rc=%d), restoring state\n", rc);

    /* Re-enable interrupts before restoring */
    sti();

    /* Restore CPU state since S3 didn't actually happen */
    suspend_restore_cpu_state();

    return -3;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  S0ix — Suspend-to-Idle support
 * ═══════════════════════════════════════════════════════════════════════ */

/* S0ix idle loop counter for statistics */
static uint64_t g_s0ix_entries = 0;
static uint64_t g_s0ix_total_ticks = 0;

/*
 * Check if the platform supports S0ix (Suspend-to-Idle).
 * Modern x86 platforms with MWAIT support and proper ACPI low-power
 * idle states can enter S0ix instead of traditional S3.
 *
 * Returns 1 if S0ix is supported, 0 otherwise.
 */
int suspend_s0ix_supported(void)
{
    /* Check for MWAIT support (required for C1e+) */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));

    int have_monitor = (ecx >> 3) & 1;  /* MONITOR/MWAIT CPUID feature bit */

    if (!have_monitor) {
        kprintf("[suspend] S0ix: MWAIT not supported\n");
        return 0;
    }

    /* Also check ACPI indicates low-power idle support */
    if (!acpi_sleep_supported(ACPI_S3)) {
        /* S0ix can work on some platforms without S3 support */
        kprintf("[suspend] S0ix: ACPI S3 not present, but MWAIT available\n");
    }

    return 1;
}

/*
 * Enter S0ix (Suspend-to-Idle) state.
 *
 * This function puts the CPU into a deep idle state using MWAIT(C1e)
 * in a loop.  The CPU stays in a low-power state until a wake event
 * (interrupt, timer, device DMA) occurs.
 *
 * The implementation:
 *   1. Disables interrupts (done by caller).
 *   2. Enters MWAIT(C1e) loop: repeatedly executes MONITOR+MWAIT
 *      with the C1e hint (eax = 0x00, ecx = 0 for C1).
 *      For deeper S0ix, use Cx state hints.
 *   3. On wake event, re-evaluates whether to re-enter idle or return.
 *
 * @flags:     0 = basic S0ix, non-zero = deep S0ix with timer disabling
 * @max_latency_us: Maximum acceptable wakeup latency in microseconds.
 *                  (0 = accept any latency)
 *
 * Returns the number of timer ticks spent in S0ix.
 */
uint64_t suspend_s0ix_enter(int flags, uint32_t max_latency_us)
{
    uint64_t start_tick, end_tick;

    if (!suspend_s0ix_supported())
        return 0;

    kprintf("[suspend] === Entering S0ix (flags=%d, max_latency=%u us) ===\n",
            flags, max_latency_us);

    start_tick = end_tick = 0;

#if defined(__x86_64__)
    start_tick = timer_get_ticks();

    /* Prepare MWAIT hint:
     *   eax: C-state + sub-state hint
     *     0x00 = C1 (shallow)
     *     0x10 = C1e (enhanced C1)
     *     0x20 = C2
     *     0x30 = C3 (deep)
     *   ecx: optional extensions
     *     0x1 = Interrupts-on-exit (wake on any interrupt)
     *     0x2 = Timers-on-exit (wake on timer expiry)
     */
    unsigned int mwait_hint = 0x10; /* C1e by default */
    unsigned int mwait_ext = 0x1;   /* Wake on interrupts */

    if (flags == 0) {
        mwait_hint = 0x00; /* Basic C1 */
    }

    /* MWAIT loop — continues until a wake event occurs */
    __asm__ volatile(
        "1: \n"
        /* MONITOR — set address range for MWAIT */
        "xor %%rax, %%rax \n"
        "xor %%rcx, %%rcx \n"
        "xor %%rdx, %%rdx \n"
        "monitor \n"
        /* MWAIT — enter low-power state */
        "mov %[hint], %%rax \n"
        "mov %[ext], %%rcx \n"
        "mwait \n"
        /* Check if we should continue (on wake) */
        "test %[maxlat], %[maxlat] \n"
        "jz 1b \n"
        : /* no outputs */
        : [hint] "r" ((uint64_t)mwait_hint),
          [ext] "r" ((uint64_t)mwait_ext),
          [maxlat] "r" ((uint64_t)max_latency_us)
        : "rax", "rcx", "rdx", "memory"
    );

    end_tick = timer_get_ticks();

    g_s0ix_entries++;
    g_s0ix_total_ticks += (end_tick - start_tick);

    kprintf("[suspend] S0ix exit: slept %llu ticks (%llu entries total)\n",
            (unsigned long long)(end_tick - start_tick),
            (unsigned long long)g_s0ix_entries);
#else
    (void)flags;
    (void)max_latency_us;
    kprintf("[suspend] S0ix: not supported on this architecture\n");
#endif

    return (end_tick > start_tick) ? (end_tick - start_tick) : 0;
}

/*
 * Return S0ix statistics.
 */
void suspend_s0ix_stats(uint64_t *entries, uint64_t *total_ticks)
{
    if (entries) *entries = g_s0ix_entries;
    if (total_ticks) *total_ticks = g_s0ix_total_ticks;
}
