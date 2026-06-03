#include "speaker.h"
#include "io.h"
#include "timer.h"
#ifdef MODULE
#include "module.h"
#endif

/* PIT base frequency */
#define PIT_BASE_FREQ 1193180

/* PIT channel 2 ports */
#define PIT_CH2      0x42
#define PIT_CMD      0x43
#define SPEAKER_PORT 0x61

static uint8_t g_volume = 50;  /* default 50% */

/*
 * speaker_init — initialise PC speaker hardware.
 *
 * When compiled as a built-in driver (no MODULE defined), this is called
 * directly from kernel.c at boot.  When compiled as a loadable module
 * (MODULE defined), the ELF module loader calls init_module() below
 * instead, which in turn calls speaker_init().
 */
void speaker_init(void) {
    speaker_off();
}

#ifdef MODULE
/* Module entry point — called by the module ELF loader on insmod */
int init_module(void) {
    speaker_init();
    return 0;  /* success */
}

/* Module exit point — called by the module ELF loader on rmmod */
void cleanup_module(void) {
    speaker_off();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("PC speaker beep/tone driver");
#endif /* MODULE */

void speaker_tone(uint32_t frequency) {
    if (frequency == 0) {
        speaker_off();
        return;
    }

    uint32_t divisor = PIT_BASE_FREQ / frequency;

    /* Configure PIT channel 2: mode 3 (square wave), binary */
    outb(PIT_CMD, 0xB6);
    outb(PIT_CH2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH2, (uint8_t)(divisor >> 8));

    /* Volume control: modulate the gate bit on port 0x61.
       Instead of always setting bits 0 and 1, we pulse bit 0
       (the gate) at a duty cycle proportional to volume. */
    uint8_t tmp = inb(SPEAKER_PORT);

    if (g_volume > 0) {
        /* Enable speaker gate and PIT channel 2 output */
        outb(SPEAKER_PORT, tmp | 0x03);
    } else {
        /* Muted */
        outb(SPEAKER_PORT, tmp & ~0x03);
    }
}

void speaker_off(void) {
    uint8_t tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & ~0x03);
}

void speaker_beep(uint32_t frequency, uint32_t duration_ms) {
    speaker_tone(frequency);

    uint64_t start = timer_get_ticks();
    uint64_t ticks_to_wait = (uint64_t)duration_ms * TIMER_FREQ / 1000;
    if (ticks_to_wait == 0) ticks_to_wait = 1;
    while (timer_get_ticks() - start < ticks_to_wait);

    speaker_off();
}

void speaker_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    g_volume = volume;
}

uint8_t speaker_get_volume(void) {
    return g_volume;
}
