/*
 * sound_core.h — Sound subsystem core: mixer interface
 *
 * Provides a unified audio mixer abstraction over hardware codecs
 * (AC97, HDA, etc.) and software-only paths (PC speaker).
 *
 * Exposes mixer controls via /sys/class/sound/ for userspace access,
 * and a simple C API for in-kernel consumers (OSS /dev/dsp, etc.).
 *
 * Item 227 — Sound core mixer interface (Plan 4, 200-more-production-improvements)
 */
#ifndef SOUND_CORE_H
#define SOUND_CORE_H

#include "types.h"

/* ── Mixer channels ──────────────────────────────────────────────── */

/** Channel identifiers for sound_mixer_set/get_volume */
enum sound_mixer_channel {
    SOUND_MIXER_MASTER = 0,       /**< Master output volume */
    SOUND_MIXER_PCM,              /**< PCM / digital audio */
    SOUND_MIXER_MIC,              /**< Microphone capture */
    SOUND_MIXER_LINE_IN,          /**< Line input */
    SOUND_MIXER_CD,               /**< CD audio input */
    SOUND_MIXER_SPEAKER,          /**< PC speaker / beep */
    SOUND_MIXER_COUNT             /**< Number of mixer channels */
};

/* ── Volume range ────────────────────────────────────────────────── */

/** Mixer volume is 0 (min/off) .. 100 (max/0dB) in the software API.
 *  The hardware codec-specific mapping is handled internally. */
#define SOUND_VOLUME_MIN   0
#define SOUND_VOLUME_MAX   100
#define SOUND_VOLUME_DEFAULT 75

/* ── Mixer state ─────────────────────────────────────────────────── */

/** Per-channel mixer state */
struct sound_mixer_channel_state {
    uint8_t  left;       /**< Left channel volume (0..100) */
    uint8_t  right;      /**< Right channel volume (0..100) */
    uint8_t  mute;       /**< 1 = muted, 0 = unmuted */
    uint8_t  recsel;     /**< 1 = selected for recording (capture) */
};

/* ── Sound core API ──────────────────────────────────────────────── */

/** Global mixer state — accessible after sound_core_init() */
extern struct sound_mixer_channel_state g_sound_mixer[SOUND_MIXER_COUNT];

/**
 * sound_core_init — Initialise the sound subsystem core.
 *
 * Must be called once at boot (after AC97 if present).
 * Creates /sys/class/sound/ hierarchy with per-channel mixer controls.
 * Registers callback hooks for hardware codec synchronisation.
 */
void sound_core_init(void);

/**
 * sound_mixer_set_volume — Set a mixer channel volume.
 * @ch:     Mixer channel (SOUND_MIXER_MASTER, etc.).
 * @left:   Left volume (0..100).
 * @right:  Right volume (0..100).
 *
 * Returns 0 on success, -1 if channel is out of range.
 * Applies to hardware codec if present, else stores in software state.
 */
int sound_mixer_set_volume(enum sound_mixer_channel ch, uint8_t left, uint8_t right);

/**
 * sound_mixer_set_mute — Mute or unmute a mixer channel.
 * @ch:    Mixer channel.
 * @mute:  1 to mute, 0 to unmute.
 *
 * Returns 0 on success, -1 if channel is out of range.
 */
int sound_mixer_set_mute(enum sound_mixer_channel ch, int mute);

/**
 * sound_mixer_set_recsel — Enable or disable recording select for a channel.
 * @ch:     Mixer channel.
 * @select: 1 to select for recording, 0 to deselect.
 *
 * Returns 0 on success, -1 if channel is out of range.
 */
int sound_mixer_set_recsel(enum sound_mixer_channel ch, int select);

/**
 * sound_mixer_read — Read a mixer channel value formatted as (left<<8|right).
 * @ch:    Mixer channel.
 *
 * Returns a 16-bit value where the low byte is left volume and the high byte
 * is right volume.  Returns 0 if channel is out of range.
 */
uint16_t sound_mixer_read(enum sound_mixer_channel ch);

/**
 * sound_mixer_write — Write a mixer channel value formatted as (left<<8|right).
 * @ch:    Mixer channel.
 * @val:   16-bit value where low byte = left volume, high byte = right volume.
 *         Bit 15 = mute (1=muted).
 *
 * Returns 0 on success, -1 if channel is out of range.
 */
int sound_mixer_write(enum sound_mixer_channel ch, uint16_t val);

#endif /* SOUND_CORE_H */
