#include "speaker.h"
#include "io.h"
#include "timer.h"
#include "string.h"
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

/* ── Console bell parameters ────────────────────────────────────── */
static uint32_t g_bell_freq = 880;   /* default ~A5 */
static uint32_t g_bell_dur  = 100;   /* 100 ms */

/* ·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─· */

/*
 * speaker_init — initialise PC speaker hardware.
 *
 * When compiled as a built-in driver (no MODULE defined), this is called
 * directly from kernel.c at boot.  When compiled as a loadable module
 * (MODULE defined), the ELF module loader calls init_module() below
 * instead, which in turn calls speaker_init().
 */
void __init speaker_init(void) {
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
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("PC speaker beep/tone driver with MIDI note and console bell support");
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

    /* Volume control: modulate the gate bit on port 0x61. */
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

/* ·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·
 * ── MIDI note interface (Item S15) ──────────────────────────────────
 * ·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─ */

/*
 * Equal temperament:  f = 440 * 2^((midi - 69) / 12)
 *
 * We compute using a pre-computed table for the 12 semitones within
 * one octave, then shift by octave.  This avoids floating-point or
 * large integer exponentiation.
 *
 * Semitone ratios (2^(0/12) .. 2^(11/12)) scaled by 2^16:
 *   ratio[i] = (uint32_t)(65536.0 * pow(2.0, i / 12.0) + 0.5)
 */
static const uint32_t semitone_ratio_x16[12] = {
    65536,  /* C       2^( 0/12) */
    69444,  /* C#/Db  2^( 1/12) */
    73590,  /* D       2^( 2/12) */
    77978,  /* D#/Eb  2^( 3/12) */
    82619,  /* E       2^( 4/12) */
    87524,  /* F       2^( 5/12) */
    92706,  /* F#/Gb  2^( 6/12) */
    98176,  /* G       2^( 7/12) */
   104037,  /* G#/Ab  2^( 8/12) */
   110198,  /* A       2^( 9/12) */
   116673,  /* A#/Bb  2^(10/12) */
   123589,  /* B       2^(11/12) */
};

/* Base frequency for MIDI note 69 (A4) in scaled form: 440 * 2^16 */
#define BASE_A4_X16 (440UL * 65536UL)

uint32_t speaker_midi_to_freq(uint8_t midi_note) {
    if (midi_note == 0)
        return 0;

    /*
     * Compute: semitone = midi_note % 12,  octave = midi_note / 12 - 1
     * (MIDI note 12 = C0, so note 12/12 - 1 = 0, i.e., octave 0)
     * Then: freq = 440 * ratio[semitone] * 2^(octave - 4) / 2^16
     *
     * For octave >= 4 we shift left, for octave < 4 we shift right.
     * We keep the arithmetic in 64-bit to avoid overflow.
     */
    int semitone = (int)(midi_note % 12);
    int octave   = (int)(midi_note / 12) - 1;

    /* Compute using the A4 reference: the semitone offset from A4 */
    int semi_offset = (int)midi_note - 69;  /* signed offset from A4 */
    if (semi_offset == 0)
        return 440;

    /* Use direct formula with the pre-computed ratios.
     * freq = 440 * ratio[semitone] / 65536 * 2^(octave - 4)
     * We compute all in 64-bit to avoid overflow. */
    uint64_t freq_x16 = BASE_A4_X16;

    /* Apply the semitone ratio for the note's semitone within its octave */
    uint64_t ratio = semitone_ratio_x16[semitone % 12];
    freq_x16 = freq_x16 * ratio / 65536UL;

    /* Shift by octave difference from A4's octave (octave 4) */
    int octave_shift = octave - 4;
    if (octave_shift >= 0) {
        freq_x16 <<= octave_shift;
    } else {
        freq_x16 >>= (-octave_shift);
    }

    /* Round to nearest Hz */
    uint32_t freq = (uint32_t)((freq_x16 + 32768) / 65536UL);
    if (freq < 20)  freq = 20;   /* below audible range */
    if (freq > 15000) freq = 15000;  /* above PC speaker capability */

    return freq;
}

void speaker_midi_note(uint8_t midi_note, uint32_t duration_ms) {
    if (midi_note == 0) {
        speaker_off();
        return;
    }
    uint32_t freq = speaker_midi_to_freq(midi_note);
    speaker_beep(freq, duration_ms);
}

/*
 * Parse a note name like "C4", "D#5", "Eb3", "F##4", "A0", "G9".
 * Format: optional accidental (b or #), note letter (A-G), optional
 * accidental again, octave digit (0-9).  Returns the MIDI note number
 * or -1 on error.
 */
static int parse_note_name(const char *name) {
    if (!name || !*name)
        return -1;

    const char *p = name;

    /* Note letter */
    int note = -1;
    switch (*p) {
        case 'C': note = 0;  break;
        case 'D': note = 2;  break;
        case 'E': note = 4;  break;
        case 'F': note = 5;  break;
        case 'G': note = 7;  break;
        case 'A': note = 9;  break;
        case 'B': note = 11; break;
        default:  return -1;
    }
    p++;

    /* Optional accidental(s) */
    int accidental = 0;
    while (*p == '#' || *p == 'b') {
        if (*p == '#') accidental++;
        if (*p == 'b') accidental--;
        p++;
        /* Max double-sharp/double-flat */
        if (accidental > 2 || accidental < -2)
            return -1;
    }

    /* Octave digit (required) */
    if (*p < '0' || *p > '9')
        return -1;
    int octave = *p - '0';
    p++;

    /* No trailing characters allowed */
    if (*p != '\0')
        return -1;

    /* Compute MIDI note number */
    int midi = note + accidental + (octave + 1) * 12;
    if (midi < 0 || midi > 127)
        return -1;

    return midi;
}

int speaker_play_note(const char *name, uint32_t duration_ms) {
    int midi = parse_note_name(name);
    if (midi < 0)
        return -1;
    speaker_midi_note((uint8_t)midi, duration_ms);
    return 0;
}

/* ·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─
 * ── Console bell ────────────────────────────────────────────────────
 * ·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─ */

void speaker_bell(void) {
    speaker_beep(g_bell_freq, g_bell_dur);
}

void speaker_set_bell_params(uint32_t freq_hz, uint32_t dur_ms) {
    if (freq_hz > 0 && freq_hz <= 15000)
        g_bell_freq = freq_hz;
    if (dur_ms > 0 && dur_ms <= 5000)
        g_bell_dur  = dur_ms;
}

#ifndef MODULE
/* Export symbols for loadable modules that call speaker functions */
#include "export.h"
EXPORT_SYMBOL(speaker_beep);
EXPORT_SYMBOL(speaker_init);
EXPORT_SYMBOL(speaker_tone);
EXPORT_SYMBOL(speaker_off);
EXPORT_SYMBOL(speaker_midi_to_freq);
EXPORT_SYMBOL(speaker_midi_note);
EXPORT_SYMBOL(speaker_play_note);
EXPORT_SYMBOL(speaker_bell);
EXPORT_SYMBOL(speaker_set_bell_params);
#endif /* !MODULE */

/* ── Turn speaker on (use last set frequency) ──────── */
int speaker_on(void)
{
    uint8_t tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp | 0x03);
    return 0;
}

/* ── Set speaker frequency via PIT channel 2 ───────── */
int speaker_set_freq(int freq)
{
    if (freq <= 0) {
        speaker_off();
        return 0;
    }

    uint32_t divisor = PIT_BASE_FREQ / (uint32_t)freq;

    /* Configure PIT channel 2: mode 3 (square wave), binary */
    outb(PIT_CMD, 0xB6);
    outb(PIT_CH2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH2, (uint8_t)(divisor >> 8));

    return 0;
}
