#ifndef DELAY_H
#define DELAY_H
#include "types.h"
#include "timer.h"
static inline void udelay(unsigned long usecs) {
    uint64_t end = timer_get_ticks() + (usecs * 100 / 10000);
    while (timer_get_ticks() < end) __asm__ volatile("pause");
}
static inline void mdelay(unsigned long msecs) {
    uint64_t end = timer_get_ticks() + (msecs * 100 / 1000);
    while (timer_get_ticks() < end) __asm__ volatile("pause");
}
#endif
