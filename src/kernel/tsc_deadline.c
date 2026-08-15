/* tsc_deadline.c — TSC deadline timer mode */

#include "cpu_features.h"
#include "cpu.h"
#include "printf.h"
#include "apic.h"
#include "idt.h"

/* Dedicated IDT vector for TSC deadline expiry interrupts.  Must not
 * collide with the PIT timer (32), keyboard (33), RTC (40), the IPI
 * vectors (0xF0-0xF5), or the spurious vector (0xFF). */
#define TSC_DEADLINE_VECTOR 0xE0

static int tsc_deadline_available = 0;
static uint64_t tsc_deadline_cached = 0;

/* IRQ handler for TSC deadline expiry.  Runs on an interrupt gate with
 * IF already cleared by hardware, so there is no interrupt state to
 * save or restore.  The critical duty is the EOI: without it the local
 * APIC in-service bit stays set and every subsequent LAPIC interrupt is
 * blocked (lost wakeups). */
static void tsc_deadline_handler(struct interrupt_frame *frame)
{
    (void)frame;
    apic_eoi();
}

int tsc_deadline_init(void) {
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for TSC deadline support (ECX bit 24) */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    if (!(rcx & (1U << 24))) {
        kprintf("[CPU] TSC deadline mode not supported\n");
        return -1;
    }

    /* Also verify x2APIC is active (TSC deadline requires either x2APIC or xAPIC) */
    uint64_t apic_base = read_msr(IA32_APIC_BASE);
    if (!(apic_base & IA32_APIC_BASE_ENABLE)) {
        kprintf("[CPU] TSC deadline: APIC not enabled\n");
        return -1;
    }

    /* Configure local APIC timer for TSC deadline mode:
     * LVT Timer register bit 18 = 1 selects TSC deadline mode */
    uint32_t lvt_timer = apic_read(LAPIC_LVT_TIMER);
    lvt_timer &= ~(3 << 17);    /* Clear timer mode bits */
    lvt_timer |= (1U << 18);     /* Set TSC deadline mode (bit 18) */
    /* Program a real vector: apic_init_local() masked the timer with
     * TIMER_MASKED (vector field 0), and an expiry on vector 0 would
     * hit the #DE divide-error gate with no EOI — the deadline wakeup
     * is lost and the LAPIC in-service bit wedges all later
     * interrupts.  Register the handler before unmasking. */
    lvt_timer = (lvt_timer & ~0xFFU) | TSC_DEADLINE_VECTOR;
    idt_register_handler_named(TSC_DEADLINE_VECTOR, tsc_deadline_handler,
                               "tsc_deadline");
    lvt_timer &= ~(1U << 16);    /* Unmask */
    apic_write(LAPIC_LVT_TIMER, lvt_timer);

    tsc_deadline_available = 1;
    kprintf("[CPU] TSC deadline timer mode enabled\n");
    return 0;
}

void tsc_deadline_set(uint64_t deadline) {
    if (!tsc_deadline_available) return;
    /* Cache the deadline — IA32_TSC_DEADLINE MSR is write-only */
    tsc_deadline_cached = deadline;
    /* Write deadline to MSR — fires interrupt when TSC >= deadline */
    write_msr(IA32_TSC_DEADLINE, deadline);
}

uint64_t tsc_deadline_get(void) {
    /* IA32_TSC_DEADLINE MSR (0x6E0) is write-only per Intel SDM
     * Vol 3 §10.5.4.1. Reading it always returns 0. Return the
     * cached value set by tsc_deadline_set() instead. */
    return tsc_deadline_cached;
}

/* ── tsc_deadline_cancel ─────────────────────────────── */
static int tsc_deadline_cancel(void)
{
    if (!tsc_deadline_available) return -1;
    /* Writing 0 to IA32_TSC_DEADLINE disarms the timer (Intel SDM) */
    write_msr(IA32_TSC_DEADLINE, 0);
    tsc_deadline_cached = 0;
    return 0;
}
/* ── tsc_deadline_read ─────────────────────────────── */
static uint64_t tsc_deadline_read(void)
{
    return tsc_deadline_cached;
}
