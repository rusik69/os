#ifndef RNG_H
#define RNG_H

#include "types.h"

/* Initialize the RNG with a seed derived from timer jitter */
void rng_init(void);

/* Return a 32-bit random value */
uint32_t rng_get_u32(void);

/* Return a 64-bit random value */
uint64_t rng_get_u64(void);

/* Fill a buffer with random bytes */
void rng_fill_buf(void *buf, uint32_t len);

/*
 * rng_add_entropy — Mix external entropy into the RNG state.
 *
 * Called by hardware RNG drivers (TPM, HWRNG, etc.) to seed or
 * re-seed the kernel's PRNG with entropy from a hardware source.
 * The data is mixed via the xorshift64 update, so even modest
 * entropy (a few bytes) improves the RNG output quality.
 *
 * @data: pointer to entropy bytes
 * @len:  number of bytes to consume
 */
void rng_add_entropy(const void *data, uint32_t len);

#endif /* RNG_H */
