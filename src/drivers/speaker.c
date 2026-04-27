#include "speaker.h"
#include "io.h"
#include "timer.h"

/* PIT base frequency */
#define PIT_BASE_FREQ 1193180

/* PIT channel 2 ports */
#define PIT_CH2      0x42
#define PIT_CMD      0x43
#define SPEAKER_PORT 0x61

void speaker_init(void) {
    /* Nothing to initialise; PC speaker is always present */
    speaker_off();
}

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

    /* Connect speaker to PIT channel 2 (bits 0 and 1 of port 0x61) */
    uint8_t tmp = inb(SPEAKER_PORT);
    if ((tmp & 0x03) != 0x03)
        outb(SPEAKER_PORT, tmp | 0x03);
}

void speaker_off(void) {
    uint8_t tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & ~0x03);
}

void speaker_beep(uint32_t frequency, uint32_t duration_ms) {
    speaker_tone(frequency);

    /* Busy-wait using timer ticks */
    uint64_t start = timer_get_ticks();
    uint64_t ticks_to_wait = (uint64_t)duration_ms * TIMER_FREQ / 1000;
    if (ticks_to_wait == 0) ticks_to_wait = 1;
    while (timer_get_ticks() - start < ticks_to_wait);

    speaker_off();
}
