#ifndef SPEAKER_H
#define SPEAKER_H

#include "types.h"

/* ── Public API ─────────────────────────────────────────────────── */

void speaker_init(void);
void speaker_tone(uint32_t frequency);
void speaker_off(void);
void speaker_beep(uint32_t frequency, uint32_t duration_ms);

/* Volume control (0 = off, 100 = max).
   Default is 50. */
void speaker_set_volume(uint8_t volume);
uint8_t speaker_get_volume(void);

/* ── MIDI note interface (Item S15) ─────────────────────────────── */

/* MIDI note numbers for octave 4 (scientific pitch notation).
 * MIDI note 0 = C-1 (8.18 Hz), note 69 = A4 (440 Hz).
 * These constants are for the middle octave; use NOTE(offset, octave)
 * to compute any note: NOTE(offset, octave) = offset + (octave - 4) * 12 + 69
 * where offset is 0=C, 2=D, 4=E, 5=F, 7=G, 9=A, 11=B. */
#define MIDI_C4  60
#define MIDI_D4  62
#define MIDI_E4  64
#define MIDI_F4  65
#define MIDI_G4  67
#define MIDI_A4  69
#define MIDI_B4  71
#define MIDI_C5  72

/* Convert a MIDI note number to frequency in Hz using equal temperament
 * (A4 = 440 Hz).  Returns 0 for note 0 (silence). */
uint32_t speaker_midi_to_freq(uint8_t midi_note);

/* Play a MIDI note for a given duration.  midi_note=0 turns off the tone. */
void speaker_midi_note(uint8_t midi_note, uint32_t duration_ms);

/* Parse a note name string (e.g. "C4", "D#5", "Eb3", "A4") and play it.
 * Returns 0 on success, -1 on parse error. */
int speaker_play_note(const char *name, uint32_t duration_ms);

/* ── Console bell ────────────────────────────────────────────────── */

/* Play the console bell sound (default ~880 Hz, 100 ms).
 * Called when the terminal outputs BEL (0x07). */
void speaker_bell(void);

/* Set a custom bell frequency and duration (in ms). */
void speaker_set_bell_params(uint32_t freq_hz, uint32_t dur_ms);

/* Note frequencies in Hz (for direct use with legacy APIs) */
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523

#endif
