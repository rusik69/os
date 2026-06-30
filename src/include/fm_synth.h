/*
 * fm_synth.h — FM synthesis engine for MIDI playback
 *
 * Implements a software 2-operator FM (Frequency Modulation) synthesiser
 * modelled after the classic Yamaha OPL architecture.  Generates 16-bit
 * signed PCM samples from MIDI note events using a carrier-modulator
 * operator pair with ADSR envelopes.
 *
 * The engine maintains up to FM_SYNTH_MAX_VOICES simultaneous voices.
 * Each voice consists of two operators (carrier + modulator) where the
 * modulator's output frequency-modulates the carrier's phase, producing
 * harmonically rich timbres.  The modulation index controls the depth
 * and therefore the brightness / spectral content.
 *
 * All frequencies use a fixed-point phase accumulator with a 1024-entry
 * sine lookup table for waveform generation.
 *
 * Task #11 — D142: MIDI synthesiser (FM synthesis baseline)
 */

#ifndef FM_SYNTH_H
#define FM_SYNTH_H

#include "types.h"

/* ── Constants ─────────────────────────────────────────────────────── */

/** Maximum simultaneous polyphonic voices */
#define FM_SYNTH_MAX_VOICES       16U

/** Size of the sine lookup table (power of two for fast indexing) */
#define FM_SYNTH_SINE_TABLE_SIZE  1024U

/** Sine table mask for wrapping */
#define FM_SYNTH_SINE_MASK        (FM_SYNTH_SINE_TABLE_SIZE - 1U)

/** Number of fractional phase bits (for fixed-point phase accumulator) */
#define FM_SYNTH_PHASE_FRAC_BITS  22U

/** Phase increment scaling: (1ULL << (FM_SYNTH_PHASE_FRAC_BITS + 10)) */
#define FM_SYNTH_PHASE_SCALE      (1ULL << 32U)

/** Default sample rate (Hz) */
#define FM_SYNTH_DEFAULT_RATE     44100U

/** MIDI note range */
#define FM_SYNTH_NOTE_MIN         0U
#define FM_SYNTH_NOTE_MAX         127U

/** Reference note (A4 = 440 Hz, MIDI note 69) */
#define FM_SYNTH_MIDI_A4          69U
#define FM_SYNTH_A4_FREQ          440UL

/* ── Waveform types ────────────────────────────────────────────────── */

enum fm_waveform {
    FM_WAVE_SINE     = 0,  /**< Pure sine (default) */
    FM_WAVE_SQUARE   = 1,  /**< Square wave (odd harmonics) */
    FM_WAVE_SAWTOOTH = 2,  /**< Sawtooth (all harmonics) */
    FM_WAVE_NOISE    = 3,  /**< Pseudo-random noise */
    FM_WAVE_COUNT,
};

/* ── ADSR envelope states ──────────────────────────────────────────── */

enum fm_env_state {
    FM_ENV_IDLE     = 0,  /**< Voice inactive / silent */
    FM_ENV_ATTACK   = 1,  /**< Ramping up to peak level */
    FM_ENV_DECAY    = 2,  /**< Falling to sustain level */
    FM_ENV_SUSTAIN  = 3,  /**< Held at sustain level */
    FM_ENV_RELEASE  = 4,  /**< Fading to zero after note-off */
};

/* ── FM operator (single oscillator + envelope) ───────────────────── */

struct fm_operator {
    /* Phase accumulator (32-bit, upper 10 bits = sine index) */
    uint32_t            phase;

    /* Phase increment per sample (frequency * PHASE_SCALE / sample_rate) */
    uint32_t            phase_inc;

    /* Envelope */
    uint8_t             env_state;      /**< Current ADSR phase */
    uint32_t            env_phase;      /**< Envelope time counter (samples) */

    /* Per-sample amplitude (0 = silent, 255 = full).  This is the
     * instantaneous envelope level. */
    uint8_t             env_level;

    /* ADSR parameters (times in samples, levels 0-255) */
    uint16_t            attack_time;    /**< Attack phase length (samples) */
    uint16_t            decay_time;     /**< Decay phase length (samples) */
    uint8_t             sustain_level;  /**< Sustain level (0-255) */
    uint16_t            release_time;   /**< Release phase length (samples) */

    /* Output level (0-255, scaled by envelope) */
    uint8_t             output_level;

    /* Waveform selection */
    uint8_t             waveform;
};

/* ── FM voice (2-operator: carrier + modulator) ───────────────────── */

struct fm_voice {
    uint8_t             active;         /**< 1 = voice is sounding */
    uint8_t             channel;        /**< MIDI channel (0-15) */
    uint8_t             note;           /**< MIDI note number (0-127) */
    uint8_t             velocity;       /**< Note-on velocity (0-127) */

    /* Modulation index controls how much the modulator affects the
     * carrier.  0 = pure sine (no modulation), 255 = max modulation. */
    uint8_t             mod_index;

    /* The two operators */
    struct fm_operator  carrier;        /**< Carrier (heard directly) */
    struct fm_operator  modulator;      /**< Modulator (modulates carrier) */
};

/* ── General MIDI instrument definition ─────────────────────────────── */

/**
 * struct fm_gm_instrument — FM synthesis parameters for one GM program.
 *
 * Each of the 128 General MIDI program numbers maps to one of these
 * structs.  When a MIDI program-change event is received (or by default
 * on channel 0), fm_setup_voice reads these parameters to configure the
 * carrier and modulator operators for the appropriate timbre.
 *
 * Frequency ratios are stored as multiplier ×2 so that integer arithmetic
 * works: ratio 2 = 1.0× (unison), 4 = 2.0× (octave), 1 = 0.5×, 3 = 1.5×
 * (fifth), etc.
 *
 * ADSR values (attack, decay, release) are 0–255 where HIGHER = FASTER
 * (shorter time, more percussive).  Sustain levels are 0–255 where 255 =
 * full sustain (held note never decays to silence).
 */
struct fm_gm_instrument {
	uint8_t car_wave;	/**< Carrier waveform  (fm_waveform enum) */
	uint8_t mod_wave;	/**< Modulator waveform (fm_waveform enum) */
	uint8_t car_ratio;	/**< Carrier  frequency multiplier ×2 */
	uint8_t mod_ratio;	/**< Modulator frequency multiplier ×2 */
	uint8_t car_level;	/**< Carrier  output level  (0–255) */
	uint8_t mod_level;	/**< Modulator output level  (0–255) */
	uint8_t mod_index;	/**< Modulation index       (0–255) */

	/* ADSR for carrier */
	uint8_t car_attack;	/**< Carrier  attack  rate (0–255, higher=shorter) */
	uint8_t car_decay;	/**< Carrier  decay   rate (0–255) */
	uint8_t car_sustain;	/**< Carrier  sustain level (0–255, 255=full) */
	uint8_t car_release;	/**< Carrier  release rate (0–255) */

	/* ADSR for modulator */
	uint8_t mod_attack;	/**< Modulator attack  rate */
	uint8_t mod_decay;	/**< Modulator decay   rate */
	uint8_t mod_sustain;	/**< Modulator sustain level */
	uint8_t mod_release;	/**< Modulator release rate */
};

/**
 * Size of the General MIDI program table.
 */
#define GM_PROGRAM_COUNT  128U

/**
 * g_fm_gm_instruments — Table of 128 GM instrument definitions.
 *
 * Indexed by MIDI program number (0–127).  The default program for
 * any channel is 0 (Acoustic Grand Piano).
 */
extern const struct fm_gm_instrument g_fm_gm_instruments[GM_PROGRAM_COUNT];

/* ── FM synthesiser state ─────────────────────────────────────────── */

struct fm_synth {
    /* Configuration */
    uint32_t            sample_rate;

    /* Polyphonic voices */
    struct fm_voice     voices[FM_SYNTH_MAX_VOICES];

    /* Per-channel MIDI program (instrument) selection */
    uint8_t             programs[16];   /**< Current GM program per channel */

    /* Global state */
    uint8_t             initialized;
    uint8_t             master_volume;  /**< 0-255 global volume */

    /* Noise LFSR state (for noise waveform) */
    uint16_t            noise_lfsr;
};

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * fm_synth_init — Initialise the FM synthesis engine.
 *
 * @sample_rate:  Desired output sample rate in Hz (e.g. 44100, 48000).
 *
 * Pre-computes the sine lookup table (if not already) and initialises
 * all voices to idle.  Must be called once before any other fm_synth_*().
 */
void fm_synth_init(uint32_t sample_rate);

/**
 * fm_synth_note_on — Start a MIDI note.
 *
 * Allocates a voice, configures its operators for the given note and
 * velocity using the current GM program for the channel, and begins
 * the attack phase.  If all voices are busy the oldest (least recently
 * started) voice is stolen.
 *
 * @channel:   MIDI channel (0-15).
 * @note:      MIDI note number (0-127, 69 = A4 = 440 Hz).
 * @velocity:  Note-on velocity (0-127, 0 is treated as note-off).
 *
 * Returns 0 on success, or a negative errno on failure.
 */
int fm_synth_note_on(uint8_t channel, uint8_t note, uint8_t velocity);

/**
 * fm_synth_note_off — Stop a MIDI note.
 *
 * Transitions the voice envelope to the release phase.  The voice is
 * freed once the release phase completes.
 *
 * @channel:   MIDI channel (0-15).
 * @note:      MIDI note number (0-127).
 */
void fm_synth_note_off(uint8_t channel, uint8_t note);

/**
 * fm_synth_all_notes_off — Immediately silence all voices.
 *
 * Transitions all active voices to IDLE instantly (panic button).
 */
void fm_synth_all_notes_off(void);

/**
 * fm_synth_program_change — Select a GM instrument for a MIDI channel.
 *
 * Sets the GM program (instrument) for the given channel.  Subsequent
 * note-on events on this channel will use the new program's FM synthesis
 * parameters (waveforms, frequency ratios, ADSR envelopes).
 *
 * Program numbers follow the General MIDI Level 1 specification (0–127).
 * Out-of-range values are silently clamped.
 *
 * @channel:  MIDI channel (0-15).
 * @program:  GM program number (0-127).
 */
void fm_synth_program_change(uint8_t channel, uint8_t program);

/**
 * fm_synth_get_program — Get the current GM program for a channel.
 *
 * @channel:  MIDI channel (0-15).
 * Returns the current GM program number (0-127).
 */
uint8_t fm_synth_get_program(uint8_t channel);

/**
 * fm_synth_render — Generate PCM audio samples.
 *
 * Renders @frames stereo frames (interleaved L/R) into @buffer.
 * Each frame is two int16_t samples (left and right).  The engine
 * processes all active voices, renders their FM output, and sums
 * them with the master volume applied.
 *
 * @buffer:  Output buffer (must hold 2 * @frames int16_t values).
 * @frames:  Number of stereo frames to generate.
 */
void fm_synth_render(int16_t *buffer, uint32_t frames);

/**
 * fm_synth_render_mono — Generate mono PCM audio samples.
 *
 * Same as fm_synth_render but produces mono output (1 sample per frame).
 *
 * @buffer:  Output buffer (must hold @frames int16_t values).
 * @frames:  Number of mono samples to generate.
 */
void fm_synth_render_mono(int16_t *buffer, uint32_t frames);

/**
 * fm_synth_active_voice_count — Return the number of currently active voices.
 */
uint32_t fm_synth_active_voice_count(void);

/**
 * fm_synth_set_master_volume — Set the global output volume.
 *
 * @vol:  Volume level (0-255, 0 = silence, 255 = full).
 */
static inline void fm_synth_set_master_volume(uint8_t vol)
{
    extern struct fm_synth g_fm_synth;
    g_fm_synth.master_volume = vol;
}

/**
 * fm_synth_get_master_volume — Get the current global output volume.
 */
static inline uint8_t fm_synth_get_master_volume(void)
{
    extern struct fm_synth g_fm_synth;
    return g_fm_synth.master_volume;
}

/* ── Global FM synthesiser instance ────────────────────────────────── */

extern struct fm_synth g_fm_synth;

/* ── Frequency conversion helper ───────────────────────────────────── */

/**
 * fm_note_to_phase_inc — Convert MIDI note number to phase increment.
 *
 * Uses equal temperament (A4 = 440 Hz).
 * The returned value is a 32-bit fixed-point phase increment suitable
 * for the FM_SYNTH_PHASE_FRAC_BITS fractional-phase system.
 *
 * @note:        MIDI note number (0-127).
 * @sample_rate: Sample rate in Hz.
 *
 * Returns the phase increment, or 0 if note is out of range / below
 * the minimum representable frequency.
 */
uint32_t fm_note_to_phase_inc(uint8_t note, uint32_t sample_rate);

/**
 * fm_freq_to_phase_inc — Convert a frequency in Hz to phase increment.
 *
 * @freq:        Frequency in Hz (as a 24.8 fixed-point value).
 * @sample_rate: Sample rate in Hz.
 *
 * Returns the 32-bit phase increment.
 */
uint32_t fm_freq_to_phase_inc(uint32_t freq_fp, uint32_t sample_rate);

#endif /* FM_SYNTH_H */
