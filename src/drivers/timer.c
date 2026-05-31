#include "timer.h"
#include "timers.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "scheduler.h"
#include "process.h"
#include "apic.h"
#include "printf.h"
#include "syscall.h" /* for timerfd_tick */

#define PIT_CMD  0x43
#define PIT_CH0  0x40

static volatile uint64_t ticks = 0;

static void timer_handler(struct interrupt_frame *frame) {
    ticks++;
    irq_ack(0);
    scheduler_wake_sleepers();
    scheduler_tick(frame->cs == 0x1b); /* was_user if CS==0x1b (ring 3) */
    int was_user = (frame->cs == 0x1b);
    process_timer_tick(was_user);
    timerfd_tick();
    posix_timer_tick();
    timer_handler_soft(); /* drive dynamic kernel timers */
    if (ticks % 200 == 0) { /* every 2 seconds: boost starved processes */
        scheduler_age();
    }
    if (ticks % TIMER_FREQ == 0) { /* every second */
        process_reap_zombies();
    }
}

void timer_init(void) {
    uint16_t divisor = 1193180 / TIMER_FREQ;

    outb(PIT_CMD, 0x36); /* channel 0, lobyte/hibyte, rate generator */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));

    idt_register_handler(32, timer_handler);

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
