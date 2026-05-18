#ifndef AC97_H
#define AC97_H

#include "types.h"

/* Initialize AC'97 audio controller (PCI class 04:01).
 * Returns 0 if found and initialized, -1 if absent. */
int ac97_init(void);

/* Play a buffer of 16-bit signed stereo PCM samples.
 * rate: sample rate in Hz (e.g. 44100).
 * len:  number of BYTES in samples[]. */
void ac97_play_pcm(const int16_t *samples, uint32_t len, uint32_t rate);

/* Returns 1 if AC'97 device is present. */
int ac97_present(void);

#endif
