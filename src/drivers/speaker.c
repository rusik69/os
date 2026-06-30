#include "speaker.h"
#include "sound_mixer_sw.h"
#include "io.h"
#include "timer.h"
#include "string.h"
#include "printf.h"
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

/* ── Audio pipeline state (PC speaker → software mixer) ─────────── */

/* Software mixer reference set by speaker_set_mixer().  When non-NULL,
 * speaker_beep() also generates PCM sine-wave samples and feeds them
 * to this mixer channel for output through the sound card. */
static struct sound_mixer_sw *g_speaker_mixer = NULL;
static int g_speaker_mixer_ch = -1;          /* -1 = not yet opened */
static int g_speaker_mixer_init_done = 0;    /* 1 = open_channel done */

/*
 * 256-entry sine lookup table covering one full cycle.
 * sin_table[i] = (int16_t)(sin(i * 2*pi / 256) * 32767)
 */
static const int16_t g_speaker_sin_table[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

/* ── Console bell parameters ────────────────────────────────────── */
static uint32_t g_bell_freq = 880;   /* default ~A5 */
static uint32_t g_bell_dur  = 100;   /* 100 ms */

/* ·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─·─· */

/*
 * speaker_pcm_fill — Fill a buffer with PCM sine-wave samples.
 *
 * @buf:          Output buffer (interleaved stereo int16_t).
 * @max_frames:   Maximum frames (sample pairs) to generate.
 * @freq_hz:      Frequency in Hz (0 = silence).
 * @sample_rate:  Sample rate in Hz (e.g. 44100).
 *
 * Generates stereo sine wave samples using the pre-computed lookup table.
 * Both channels get the same signal at a reduced amplitude (-6 dB) to
 * leave headroom for software mixing.
 *
 * Returns the number of frames written (always == max_frames unless
 * max_frames == 0, in which case 0 is returned).
 */
uint32_t speaker_pcm_fill(int16_t *buf, uint32_t max_frames,
                          uint32_t freq_hz, uint32_t sample_rate)
{
    if (!buf || max_frames == 0)
        return 0;
    if (freq_hz == 0 || sample_rate == 0) {
        memset(buf, 0, (size_t)max_frames * 2 * sizeof(int16_t));
        return max_frames;
    }

    /*
     * Phase increment:  how many table entries to advance per sample.
     * table_size = 256, so:
     *   phase_inc = freq_hz * 256 / sample_rate
     * Stored as 16.16 fixed-point for fractional stepping.
     */
    uint32_t phase_inc = (uint32_t)(((uint64_t)freq_hz << 16) / (uint64_t)sample_rate);
    uint32_t phase = 0;

    /* Amplitude: 16384 ≈ -6 dB — leaves headroom for multi-stream mixing */
    const int32_t amp = 16384;

    for (uint32_t i = 0; i < max_frames; i++) {
        uint32_t idx = (phase >> 16) & 0xFF;
        uint32_t frac = phase & 0xFFFF;
        uint32_t next = (idx + 1) & 0xFF;

        /* Linear interpolation between adjacent table entries */
        int32_t s0 = (int32_t)g_speaker_sin_table[idx];
        int32_t s1 = (int32_t)g_speaker_sin_table[next];
        int32_t sample = s0 + ((s1 - s0) * (int32_t)frac >> 16);
        sample = (sample * amp) >> 15;

        /* Stereo interleaved: same signal on both channels */
        buf[i * 2]     = (int16_t)sample;
        buf[i * 2 + 1] = (int16_t)sample;

        phase += phase_inc;
    }

    return max_frames;
}

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

    /* ── Audio pipeline path ────────────────────────────────────────
     * During the beep duration, generate PCM sine-wave samples and
     * feed them to the software mixer so the beep is also heard
     * through the sound card.                                 */
    if (g_speaker_mixer && frequency > 0 && duration_ms > 0) {
        uint32_t sample_rate = 44100;
        uint32_t total_frames = (uint32_t)(
            (uint64_t)duration_ms * (uint64_t)sample_rate / 1000ULL);

        /*
         * If the mixer channel hasn't been opened yet, open it now.
         * This lazy-open avoids ordering dependencies at boot — the
         * mixer can be registered after speaker_init().
         */
        if (!g_speaker_mixer_init_done) {
            int ch = sound_mixer_sw_open_channel(g_speaker_mixer, 192);
            if (ch >= 0) {
                g_speaker_mixer_ch = ch;
            }
            g_speaker_mixer_init_done = 1;
        }

        if (g_speaker_mixer_ch >= 0) {
            /*
             * Write PCM data in small chunks during the busy-wait
             * loop.  Each chunk is 128 frames (about 2.9 ms at
             * 44.1 kHz) to keep stack usage modest.
             */
            uint32_t chunk_frames = 128;
            uint32_t written = 0;

            while (timer_get_ticks() - start < ticks_to_wait &&
                   written < total_frames) {
                uint32_t todo = total_frames - written;
                if (todo > chunk_frames)
                    todo = chunk_frames;

                int16_t buf[256]; /* 128 frames * 2 channels */
                speaker_pcm_fill(buf, todo, frequency, sample_rate);

                int ret = sound_mixer_sw_write(
                    g_speaker_mixer, g_speaker_mixer_ch, buf, todo);
                if (ret > 0)
                    written += (uint32_t)ret;
                else
                    break; /* mixer full, stop writing */
            }
        }
    }

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

/* ── Audio pipeline integration ───────────────────────────────────── */

void speaker_set_mixer(struct sound_mixer_sw *mixer)
{
    /* If a mixer channel was already opened and we're changing mixer,
     * close the old channel first. */
    if (mixer != g_speaker_mixer && g_speaker_mixer && g_speaker_mixer_ch >= 0) {
        sound_mixer_sw_close_channel(g_speaker_mixer, g_speaker_mixer_ch);
        g_speaker_mixer_ch = -1;
        g_speaker_mixer_init_done = 0;
    }

    g_speaker_mixer = mixer;

    /* If mixer is being cleared (NULL), also reset the channel state. */
    if (!mixer) {
        g_speaker_mixer_ch = -1;
        g_speaker_mixer_init_done = 0;
    }
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
EXPORT_SYMBOL(speaker_pcm_fill);
EXPORT_SYMBOL(speaker_set_mixer);
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
