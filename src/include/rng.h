#ifndef RNG_H
#define RNG_H

#include "types.h"

/* Initialize the RNG with a seed derived from timer jitter
 * and hardware entropy sources (RDRAND/RDSEED if available). */
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

/*
 * Hardware RNG availability
 * --------------------------
 * Returns non-zero if the CPU supports RDRAND (Intel Ivy Bridge+ / AMD
 * Jaguar+) which provides a hardware random number generator suitable
 * for seeding kernel PRNGs and cryptographic operations.
 */
int rng_hw_rdrand_available(void);

/*
 * Seed the kernel RNG with entropy from RDRAND / RDSEED.
 *
 * Attempts to read @words 64-bit values from RDRAND (or, if available,
 * the more robust RDSEED instruction) and mixes them into the RNG
 * state via rng_add_entropy().  Returns the number of words actually
 * obtained, or 0 if no hardware RNG is available.
 *
 * @words:  number of 64-bit values to attempt (capped at 16)
 * @flags:  0 = prefer RDRAND, bit 0 = prefer RDSEED if available
 */
int rng_seed_from_hw(int words, int flags);

/* Flags for rng_seed_from_hw */
#define RNG_HW_PREFER_RDSEED  1  /* prefer RDSEED over RDRAND */

/*
 * rng_get_random — Fill a kernel buffer with random bytes.
 *
 * Returns the number of bytes written on success, or a negative
 * errno on error.  The buffer is filled from the kernel PRNG
 * (xorshift64 seeded from timer jitter + hardware entropy).
 *
 * @buf:   kernel buffer to fill
 * @count: number of random bytes requested
 */
int rng_get_random(void *buf, size_t count);

/*
 * Flags for sys_getrandom (GRND_*)
 * Compatible with Linux include/uapi/linux/random.h
 */
#define GRND_NONBLOCK  1  /* don't block if insufficient entropy */
#define GRND_RANDOM    2  /* use /dev/random (blocking) pool */

#endif /* RNG_H */
