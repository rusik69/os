/*
 * suspend.c — ACPI S3 Suspend-to-RAM support
 *
 * Implements the OS side of ACPI S3 (Suspend-to-RAM):
 *   - Save CPU/system state before entering S3
 *   - Enter S3 via PM1a_CNT (delegated to ACPI driver)
 *   - Restore CPU/system state on wake
 *
 * References:
 *   ACPI Specification v6.5, Section 4.8.4 — System Sleep (S3)
 *   Intel 64 and IA-32 Architectures SDM, Vol 3A, Chapter 9 — SMM
 */

#include "suspend.h"
#include "acpi.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "io.h"

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
