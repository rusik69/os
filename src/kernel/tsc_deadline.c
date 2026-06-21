/* tsc_deadline.c — TSC deadline timer mode */

#include "cpu_features.h"
#include "cpu.h"
#include "printf.h"
#include "apic.h"

static int tsc_deadline_available = 0;

int tsc_deadline_init(void) {
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 1 for TSC deadline support (ECX bit 24) */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(1));

    if (!(rcx & (1 << 24))) {
        kprintf("[cpu] TSC deadline mode not supported\n");
        return -1;
    }

    /* Also verify x2APIC is active (TSC deadline requires either x2APIC or xAPIC) */
    uint64_t apic_base = read_msr(IA32_APIC_BASE);
    if (!(apic_base & IA32_APIC_BASE_ENABLE)) {
        kprintf("[cpu] TSC deadline: APIC not enabled\n");
        return -1;
    }

    /* Configure local APIC timer for TSC deadline mode:
     * LVT Timer register bit 18 = 1 selects TSC deadline mode */
    uint32_t lvt_timer = apic_read(LAPIC_LVT_TIMER);
    lvt_timer &= ~(3 << 17);    /* Clear timer mode bits */
    lvt_timer |= (1 << 18);     /* Set TSC deadline mode (bit 18) */
    lvt_timer &= ~(1 << 16);    /* Unmask */
    apic_write(LAPIC_LVT_TIMER, lvt_timer);

    tsc_deadline_available = 1;
    kprintf("[cpu] TSC deadline timer mode enabled\n");
    return 0;
}

void tsc_deadline_set(uint64_t deadline) {
    if (!tsc_deadline_available) return;
    /* Write deadline to MSR — fires interrupt when TSC >= deadline */
    write_msr(IA32_TSC_DEADLINE, deadline);
}

uint64_t tsc_deadline_get(void) {
    return read_msr(IA32_TSC_DEADLINE);
}

/* ── Stub: tsc_deadline_cancel ─────────────────────────────── */
int tsc_deadline_cancel(void)
{
    kprintf("[tsc] tsc_deadline_cancel: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: tsc_deadline_read ─────────────────────────────── */
uint64_t tsc_deadline_read(void)
{
    kprintf("[tsc] tsc_deadline_read: not yet implemented\n");
    return -ENOSYS;
}
