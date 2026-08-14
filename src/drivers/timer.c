/*
 * src/drivers/timer.c — PIT-based system timer.
 *
 * ==========================================================================
 * Architecture Overview
 * ==========================================================================
 *
 * This file implements the system timer tick using the Intel 8253/8254
 * Programmable Interval Timer (PIT), the legacy timer chip present on all
 * x86 PC-compatible systems.  The PIT drives the kernel's timekeeping,
 * scheduler ticks, per-process POSIX timers (SIGALRM, setitimer), timerfd
 * events, dynamic timers, RCU stall detection, and NO_HZ idle accounting.
 *
 * PIT Hardware Details
 * --------------------
 * The PIT contains three independent 16-bit counter channels:
 *   - Channel 0 (IRQ 0):  System timer tick (used here).
 *   - Channel 1:          DRAM refresh (legacy, usually not used).
 *   - Channel 2:          PC speaker tone generation.
 *
 * Each channel has a 16-bit down-counter that decrements at the PIT
 * base frequency of 1.193181666... MHz (1/3 of the 14.31818 MHz NTSC
 * colour-burst crystal).  When the counter reaches zero, it fires an
 * interrupt and (in mode 2) reloads the divisor and continues.
 *
 * Initialisation Sequence (timer_init)
 * -------------------------------------
 * 1.  Compute the 16-bit divisor: divisor = 1193180 / TIMER_FREQ.
 *     With TIMER_FREQ = 100 Hz, divisor = 11931 (0x2E9B).
 *
 * 2.  Write PIT command byte 0x34 to port 0x43:
 *       - Bits 7-6 (00):  Counter 0 select.
 *       - Bits 5-4 (11):  Access mode — lobyte then hibyte.
 *       - Bits 3-1 (010): Mode 2 — rate generator.
 *       - Bit  0  (0):    16-bit binary counter.
 *
 * 3.  Write divisor LSB then MSB to PIT channel 0 data port (0x40).
 *
 * 4.  Register timer_handler as the IRQ 0 interrupt handler via the IDT.
 *
 * Interrupt Routing
 * -----------------
 * The PIT Channel 0 output is connected to IRQ 0 on the legacy 8259A PIC.
 * When the I/O APIC is available (apic_is_init_complete() returns true),
 * the kernel uses ExtINT delivery mode:
 *
 *   PIT -> PIC (master IRQ 0) -> CPU PIC output -> I/O APIC LINT0
 *                   (ExtINT)                      (via ioapic_redirect_extint)
 *
 * The I/O APIC is programmed to forward whatever the PIC generates as an
 * ExtINT-message-signalled interrupt to the BSP's local APIC LINT0.
 * Both the PIC master IRQ 0 and the I/O APIC pin 0 are unmasked.  The
 * local APIC LVT LINT0 entry is set to ExtINT delivery (vector 7 << 8).
 *
 * If the I/O APIC is not yet initialised, the legacy PIC unmask is used
 * alone, and the interrupt arrives through the PIC's direct connection
 * to the CPU INTR pin (not through the local APIC).
 *
 * Tick Handler (timer_handler)
 * ----------------------------
 * Called on every timer interrupt (IRQ 0, ~100 Hz).  The handler:
 *   1.  Increments the global tick counter (ticks).
 *   2.  Acknowledges the interrupt (irq_ack(0)).
 *   3.  Determines whether the interrupted context was user or kernel
 *       by checking the CS selector (0x1B == user, 0x08/0x10 == kernel).
 *   4.  Accounts the tick to the NO_HZ subsystem (nohz_tick_account).
 *   5.  Invokes scheduler_tick() to update process timeslices, handle
 *       round-robin preemption, and trigger load balancing.
 *   6.  Raises the TIMER softirq so that registered dynamic timers,
 *       timerfd expirations, and POSIX per-process timers are dispatched
 *       in a context with interrupts enabled.
 *   7.  Re-enables interrupts (sti()) and runs pending softirqs
 *       (do_softirq()).
 *
 * Timer Query Functions
 * ---------------------
 *   timer_get_ticks()  — Returns the raw tick counter value.
 *   timer_get_ns()     — Converts ticks to nanoseconds with overflow
 *                         saturation (ticks * NS_PER_TICK).
 *   timer_get_ms()     — Converts ticks to milliseconds with overflow
 *                         saturation (ticks * (NS_PER_TICK / 1,000,000)).
 *
 * Stubs / Future Work
 * -------------------
 *   timer_read()        — Stub: intended for microsecond/granular read.
 *   timer_set_period()  — Stub: intended for runtime frequency change.
 *   timer_get_freq()    — Stub: intended to report current frequency.
 *
 * HPET (High Precision Event Timer)
 * ---------------------------------
 * HPET is not yet implemented in this kernel.  When added, HPET would
 * provide a higher-resolution alternative to the PIT (≥10 MHz vs 1.19 MHz)
 * with multiple comparators and better power-management support.  The
 * timer abstraction layer will allow runtime selection between PIT and
 * HPET as the clock event device.
 */
#define KERNEL_INTERNAL

#include "timer.h"

#include "apic.h"
#include "export.h"
#include "idt.h"
#include "io.h"
#include "net.h" /* net_poll — network stack polling on each tick */
#include "nmi_watchdog.h"
#include "nohz.h"
#include "pic.h"
#include "printf.h"
#include "process.h"
#include "rcu.h" /* rcu_check_stall */
#include "scheduler.h"
#include "softirq.h"
#include "syscall.h" /* timerfd_tick, posix_timer_tick */
#include "timers.h"
#include "vsyscall.h"

#define PIT_CMD 0x43
#define PIT_CH0 0x40

static volatile uint64_t ticks = 0;

static void timer_handler(struct interrupt_frame *frame) {
    ticks++;
    irq_ack(0);
    int was_user = (frame->cs == 0x1b);

    /* Account this tick to the NO_HZ subsystem */
    nohz_tick_account(0); /* CPU 0 handles the timer; tick state tracked globally */

    scheduler_tick(was_user);

    /* Drive POSIX per-process timer expiry (timer_create/timer_settime).
     * The 89aade74 boot-sequence rework dropped this call and it was never
     * re-wired: without it, armed POSIX timers never expire, never deliver
     * their expiry signal, and one-shot slots never disarm (leaking the
     * slot).  signal_send() is safe from this context — scheduler_tick's
     * RLIMIT_CPU enforcement already delivers signals from here. */
    posix_timer_tick();

    /* NOTE: the NIC is NOT polled from the timer.  The netd kthread
     * (telnetd_task) drains the NIC in process context — running the full
     * TCP stack on the IRQ stack re-enters tcp_lock from the timer's own
     * send path and deadlocks. */

    softirq_raise(SOFTIRQ_TIMER);
    sti();
    do_softirq();
}

void __init timer_init(void) {
    uint16_t divisor = 1193180 / TIMER_FREQ;

    outb(PIT_CMD, 0x34); /* channel 0, lobyte/hibyte, rate generator (mode 2) */
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
static uint64_t timer_read(void) {
    kprintf("[timer] timer_read: not yet implemented\n");
    return 0;
}
/* ── Stub: timer_set_period ─────────────────────────────── */
static int timer_set_period(uint64_t period) {
    (void)period;
    kprintf("[timer] timer_set_period: not yet implemented\n");
    return 0;
}
/* ── Stub: timer_get_freq ─────────────────────────────── */
static uint64_t timer_get_freq(void) {
    kprintf("[timer] timer_get_freq: not yet implemented\n");
    return 0;
}
