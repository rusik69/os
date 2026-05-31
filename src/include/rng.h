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

#endif /* RNG_H */
