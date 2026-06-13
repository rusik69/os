/*
 * sndstat.c — /proc/asound summary (S18)
 *
 * Provides a read-only /proc/asound virtual file that reports the
 * state of the sound subsystem: cards, devices, PCM streams, and
 * timers.
 *
 * Integrates with:
 *   - AC97 driver (ac97.c) — audio controller state
 *   - OSS driver (sound_oss.c) — /dev/dsp state
 *   - Sound core mixer (sound_core.c) — mixer state
 */

#include "ac97.h"
#include "sound_core.h"
#include "string.h"
#include "printf.h"
#include "types.h"

/* ── Forward declarations ──────────────────────────────────────────── */

/* These are called from procfs when /proc/asound is read. */

/**
 * sndstat_generate — Generate /proc/asound content.
 * @buf:  Output buffer.
 * @size: Maximum buffer size.
 *
 * Returns the number of bytes written to buf (excluding null terminator).
 */
int sndstat_generate(char *buf, int size)
{
    int pos = 0;
    int n;

    /* ── Header ──────────────────────────────────────────────────── */
    n = snprintf(buf + pos, (size_t)(size - pos), "Sound Driver Summary\n");
    if (n > 0 && pos + n < size) pos += n;

    /* ── Cards ────────────────────────────────────────────────────── */
    n = snprintf(buf + pos, (size_t)(size - pos), "\nCards:\n");
    if (n > 0 && pos + n < size) pos += n;

    if (ac97_present()) {
        n = snprintf(buf + pos, (size_t)(size - pos),
                     "  Card 0: AC'97 Audio Controller\n"
                     "    Type: AC97\n"
                     "    Status: %s\n",
                     ac97_present() ? "active" : "absent");
    } else {
        n = snprintf(buf + pos, (size_t)(size - pos),
                     "  No sound cards found.\n");
    }
    if (n > 0 && pos + n < size) pos += n;

    /* ── Devices ──────────────────────────────────────────────────── */
    n = snprintf(buf + pos, (size_t)(size - pos), "\nDevices:\n");
    if (n > 0 && pos + n < size) pos += n;

    n = snprintf(buf + pos, (size_t)(size - pos),
                 "  Card 0:\n"
                 "    Device 0: [AC97 Audio] (Mixer)\n"
                 "    Device 1: [AC97 Audio] (PCM)\n");
    if (n > 0 && pos + n < size) pos += n;

    /* ── PCM Streams ──────────────────────────────────────────────── */
    n = snprintf(buf + pos, (size_t)(size - pos), "\nPCM Streams:\n");
    if (n > 0 && pos + n < size) pos += n;

    n = snprintf(buf + pos, (size_t)(size - pos),
                 "   Card 0:\n"
                 "    PCM 0: playback (stereu, 16-bit, 44100 Hz max)\n"
                 "    PCM 1: capture  (mono,  16-bit, 44100 Hz max)\n");
    if (n > 0 && pos + n < size) pos += n;

    /* ── Mixer channels ───────────────────────────────────────────── */
    n = snprintf(buf + pos, (size_t)(size - pos), "\nMixer Controls:\n");
    if (n > 0 && pos + n < size) pos += n;

    for (int ch = 0; ch < SOUND_MIXER_COUNT; ch++) {
        uint16_t vol = sound_mixer_read((enum sound_mixer_channel)ch);
        uint8_t left  = vol & 0xFF;
        uint8_t right = (vol >> 8) & 0xFF;

        static const char * const ch_names[] = {
            "Master", "PCM", "Mic", "LineIn", "CD", "Speaker"
        };
        const char *ch_name = (ch < 6) ? ch_names[ch] : "Unknown";

        n = snprintf(buf + pos, (size_t)(size - pos),
                     "  %-8s: L=%3u R=%3u %s\n",
                     ch_name,
                     (unsigned int)left,
                     (unsigned int)right,
                     g_sound_mixer[ch].mute ? "[MUTED]" : "");
        if (n > 0 && pos + n < size) pos += n;
    }

    /* ── Timers ───────────────────────────────────────────────────── */
    n = snprintf(buf + pos, (size_t)(size - pos), "\nTimers:\n");
    if (n > 0 && pos + n < size) pos += n;

    n = snprintf(buf + pos, (size_t)(size - pos),
                 "  System timer (system): 1 (system timer IRQ)\n"
                 "  PC Speaker (speaker):  1 (PIT)\n");
    if (n > 0 && pos + n < size) pos += n;

    /* Ensure null termination */
    if (pos < size)
        buf[pos] = '\0';
    else if (size > 0)
        buf[size - 1] = '\0';

    return pos;
}
