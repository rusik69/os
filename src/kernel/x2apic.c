/* x2apic.c — x2APIC detection and enable */

#include "cpu_features.h"
#include "cpu.h"
#include "printf.h"
#include "apic.h"

static int x2apic_active = 0;

/*
 * x2APIC MSRs — in x2APIC mode each LAPIC register is accessed via a
 * dedicated MSR at 0x800 + (MMIO_offset >> 4).
 *
 * LAPIC_TPR      (0x080) → MSR 0x808    LAPIC_EOI     (0x0B0) → MSR 0x80B
 * LAPIC_LDR      (0x0D0) → MSR 0x80D    LAPIC_SVR     (0x0F0) → MSR 0x80F
 * LAPIC_ICR_LOW  (0x300) → MSR 0x830    LAPIC_ICR_HIGH (0x310) → MSR 0x830 (64-bit)
 * LAPIC_LVT_TIMER(0x320) → MSR 0x832    LAPIC_LVT_PC  (0x340) → MSR 0x834
 * LAPIC_LVT_LINT0(0x350) → MSR 0x835    LAPIC_LVT_LINT1(0x360) → MSR 0x836
 * LAPIC_LVT_ERROR(0x370) → MSR 0x837    LAPIC_TMICT   (0x380) → MSR 0x838
 * LAPIC_TMCURR   (0x390) → MSR 0x839    LAPIC_TMDIV   (0x3E0) → MSR 0x83E
 * LAPIC_DFR      (0x0E0) — NOT PRESENT in x2APIC (always flat logical)
 */
#define X2APIC_MSR(reg)  (0x800U + ((reg) >> 4))

/* Write a LAPIC register directly via the x2APIC MSR interface.
 * Only valid after x2APIC mode has been enabled. */
static inline void x2apic_write_reg(uint32_t reg, uint32_t val)
{
    write_msr(X2APIC_MSR(reg), val);
}

/* Read a LAPIC register directly via the x2APIC MSR interface. */
static inline uint32_t x2apic_read_reg(uint32_t reg)
{
    return (uint32_t)read_msr(X2APIC_MSR(reg));
}

int __init x2apic_init(void) {
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for x2APIC support (ECX bit 21) */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    if (!(rcx & (1U << 21))) {
        kprintf("[CPU] x2APIC not supported\n");
        return -1;
    }

    kprintf("[CPU] x2APIC supported by CPU\n");

    /* Read current APIC base MSR */
    uint64_t apic_base = read_msr(IA32_APIC_BASE);

    /* Check if already in x2APIC mode */
    if (apic_base & IA32_APIC_BASE_X2APIC) {
        kprintf("[CPU] x2APIC already active\n");
        x2apic_active = 1;
        return 0;
    }

    /* ── Transition from xAPIC to x2APIC mode ──────────────────────
     *
     * Per Intel SDM Vol 3, 10.12.5.1:
     *
     *   a) Software-disabling the APIC (IA32_APIC_BASE[11] = 0) drops
     *      ALL pending IRR interrupts. We therefore MUST:
     *        - Mask all LVT sources before the transition
     *        - Disable interrupts (CLI) to prevent new ones arriving
     *        - Acknowledge any in-service interrupt (EOI)
     *
     *   b) After re-enabling in x2APIC mode, ALL LAPIC registers are
     *      at their reset / default values. We save every register
     *      we configured earlier (via MMIO in apic_init_local()) and
     *      restore them via the x2APIC MSR interface after the switch.
     *
     *   c) The ICR becomes a single 64-bit MSR (0x830) in x2APIC mode,
     *      combining the old ICR_LOW + ICR_HIGH.  apic_send_ipi() and
     *      friends are updated separately to use the x2APIC path when
     *      x2apic_is_active() returns true.
     *
     *   d) DFR (Destination Format Register) does not exist in x2APIC
     *      mode — logical addressing is always flat.
     *
     *   e) LDR semantics change: in xAPIC flat mode the register holds
     *      a bitmask (1 << apic_id) << 24; in x2APIC mode it holds the
     *      full 32-bit logical x2APIC ID directly.
     */

    /* ── Step 1: Save current LAPIC state (MMIO reads) ───────────── */
    uint32_t saved_svr      = apic_read(LAPIC_SVR);
    uint32_t saved_tpr      = apic_read(LAPIC_TPR);
    uint32_t saved_ldr      = apic_read(LAPIC_LDR);
    uint32_t saved_lvt_timer = apic_read(LAPIC_LVT_TIMER);
    uint32_t saved_lvt_pc   = apic_read(LAPIC_LVT_PC);
    uint32_t saved_lvt_lint0 = apic_read(LAPIC_LVT_LINT0);
    uint32_t saved_lvt_lint1 = apic_read(LAPIC_LVT_LINT1);
    uint32_t saved_lvt_err  = apic_read(LAPIC_LVT_ERROR);
    uint32_t saved_tmict    = apic_read(LAPIC_TMICT);
    uint32_t saved_tmdiv    = apic_read(LAPIC_TMDIV);

    /* ── Step 2: Mask all LVTs to prevent spurious delivery ──────── */
    apic_write(LAPIC_LVT_TIMER, saved_lvt_timer | TIMER_MASKED);
    apic_write(LAPIC_LVT_PC,    saved_lvt_pc    | TIMER_MASKED);
    apic_write(LAPIC_LVT_LINT0, saved_lvt_lint0 | TIMER_MASKED);
    apic_write(LAPIC_LVT_LINT1, saved_lvt_lint1 | TIMER_MASKED);
    apic_write(LAPIC_LVT_ERROR, saved_lvt_err   | TIMER_MASKED);
    __asm__ volatile("mfence" ::: "memory");

    /* ── Step 3: Disable local interrupts ────────────────────────── */
    __asm__ volatile("cli" ::: "memory");

    /* ── Step 4: Acknowledge any in-service interrupt ────────────── */
    apic_write(LAPIC_EOI, 0);
    __asm__ volatile("mfence" ::: "memory");

    /* ── Step 5: Perform the actual mode switch ────────────────────
     * Order matters: disable APIC first, then set x2APIC bit,
     * then re-enable.  Switching directly from xAPIC to x2APIC
     * without the intermediate disabled state is undefined. */
    apic_base &= ~IA32_APIC_BASE_ENABLE;  /* Disable APIC (drops IRR) */
    write_msr(IA32_APIC_BASE, apic_base);

    apic_base |= IA32_APIC_BASE_X2APIC;   /* Enable x2APIC */
    apic_base |= IA32_APIC_BASE_ENABLE;   /* Re-enable APIC */
    write_msr(IA32_APIC_BASE, apic_base);

    __asm__ volatile("mfence" ::: "memory");

    /* ── Step 6: Restore LAPIC state via x2APIC MSRs ───────────────
     * Registers are at reset values — we must write everything.
     * Order: TPR → SVR → LVTs → LDR → timer. */

    /* Task priority (affects interrupt acceptance) */
    x2apic_write_reg(LAPIC_TPR, saved_tpr);

    /* Spurious vector (must have ENABLE set for interrupts to work) */
    x2apic_write_reg(LAPIC_SVR, saved_svr);

    /* LVT entries — restore unmasked state */
    x2apic_write_reg(LAPIC_LVT_TIMER, saved_lvt_timer);
    x2apic_write_reg(LAPIC_LVT_PC,    saved_lvt_pc);
    x2apic_write_reg(LAPIC_LVT_LINT0, saved_lvt_lint0);
    x2apic_write_reg(LAPIC_LVT_LINT1, saved_lvt_lint1);
    x2apic_write_reg(LAPIC_LVT_ERROR, saved_lvt_err);

    /* Logical destination register.
     * In x2APIC flat mode LDR holds the logical x2APIC ID directly.
     * We read the x2APIC ID from MSR 0x802 (LAPIC_ID >> 4). */
    uint32_t x2apic_id = x2apic_read_reg(LAPIC_ID);
    /* Set logical destination to match the physical x2APIC ID,
     * preserving logical-addressing reachability. */
    x2apic_write_reg(LAPIC_LDR, x2apic_id);

    /* Timer initial count and divider */
    x2apic_write_reg(LAPIC_TMICT, saved_tmict);
    x2apic_write_reg(LAPIC_TMDIV, saved_tmdiv);

    /* Acknowledge any residual interrupt */
    x2apic_write_reg(LAPIC_EOI, 0);

    __asm__ volatile("mfence" ::: "memory");

    /* ── Step 7: Re-enable local interrupts ──────────────────────── */
    __asm__ volatile("sti" ::: "memory");

    /* ── Step 8: Verify the transition ───────────────────────────── */
    uint64_t check = read_msr(IA32_APIC_BASE);
    if (check & IA32_APIC_BASE_X2APIC) {
        kprintf("[CPU] x2APIC enabled successfully\n");
        x2apic_active = 1;
        return 0;
    } else {
        kprintf("[CPU] x2APIC enable FAILED\n");
        return -1;
    }
}

int x2apic_is_active(void) {
    return x2apic_active;
}
