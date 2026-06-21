/*
 * src/drivers/timer.c — PIT-based system timer.
 *
 * Drives the scheduler, dynamic timers, timerfd, and POSIX per-process
 * timers on every tick.  Handles PIT → PIC → I/O APIC ExtINT routing.
 */
#define KERNEL_INTERNAL

#include "timer.h"
#include "timers.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "scheduler.h"
#include "process.h"
#include "apic.h"
#include "printf.h"
#include "syscall.h"   /* timerfd_tick, posix_timer_tick */
#include "rcu.h"       /* rcu_check_stall */
#include "nmi_watchdog.h"
#include "vsyscall.h"
#include "nohz.h"
#include "export.h"

#define PIT_CMD  0x43
#define PIT_CH0  0x40

static volatile uint64_t ticks = 0;

static void timer_handler(struct interrupt_frame *frame) {
    ticks++;
    irq_ack(0);
    int was_user = (frame->cs == 0x1b);

    /* Account this tick to the NO_HZ subsystem */
    nohz_tick_account(0);  /* CPU 0 handles the timer; tick state tracked globally */

    scheduler_tick(was_user);
}

void timer_init(void) {
    uint16_t divisor = 1193180 / TIMER_FREQ;

    outb(PIT_CMD, 0x36); /* channel 0, lobyte/hibyte, rate generator */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));

    idt_register_handler_named(32, timer_handler, "timer");

    /* Legacy PIC → ExtINT → I/O APIC routing.
     * On many chipsets the PIT output is only connected to the PIC,
     * not directly to the I/O APIC.  Using ExtINT delivery makes the
     * I/O APIC forward whatever the legacy PIC generates. */
    if (apic_is_init_complete()) {
        ioapic_redirect_extint(0);
        ioapic_unmask_irq(0);
        pic_unmask(0);
        /* Unmask the local APIC LINT0 for ExtINT delivery so the PIC's
         * interrupts actually reach the CPU through the local APIC. */
        apic_write(LAPIC_LVT_LINT0, (7 << 8)); /* ExtINT, unmasked */
    } else {
        pic_unmask(0);
    }
}

uint64_t timer_get_ticks(void) {
    return ticks;
}
EXPORT_SYMBOL(timer_get_ticks);

uint64_t timer_get_ns(void) {
    /* Use inline multiply with overflow check: ticks * 10,000,000 ns/tick */
    uint64_t t = ticks;
    /* NS_PER_TICK = 10,000,000; check for overflow */
    if (t > (uint64_t)(-1ULL) / NS_PER_TICK)
        return (uint64_t)(-1ULL); /* saturate on overflow */
    return t * NS_PER_TICK;
}

uint64_t timer_get_ms(void) {
    /* ticks * 10  (since NS_PER_TICK / 1,000,000 = 10) */
    uint64_t t = ticks;
    if (t > (uint64_t)(-1ULL) / 10ULL)
        return (uint64_t)(-1ULL); /* saturate on overflow */
    return t * 10ULL;
}

/* ── Stub: timer_read ─────────────────────────────── */
uint64_t timer_read(void)
{
    kprintf("[timer] timer_read: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: timer_set_period ─────────────────────────────── */
int timer_set_period(uint64_t period)
{
    (void)period;
    kprintf("[timer] timer_set_period: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: timer_get_freq ─────────────────────────────── */
uint64_t timer_get_freq(void)
{
    kprintf("[timer] timer_get_freq: not yet implemented\n");
    return -ENOSYS;
}
