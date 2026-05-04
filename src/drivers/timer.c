#include "timer.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "scheduler.h"
#include "process.h"

#define PIT_CMD  0x43
#define PIT_CH0  0x40

static volatile uint64_t ticks = 0;

static void timer_handler(struct interrupt_frame *frame) {
    (void)frame;
    ticks++;
    pic_eoi(0);
    scheduler_wake_sleepers();
    if (ticks % 5 == 0) { /* every 50ms */
        schedule();
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
    pic_unmask(0);
}

uint64_t timer_get_ticks(void) {
    return ticks;
}
