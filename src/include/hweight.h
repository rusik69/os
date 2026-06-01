#ifndef HWEIGHT_H
#define HWEIGHT_H

#include "types.h"

/* Population count (Hamming weight) — number of set bits. */

unsigned int hweight8(uint8_t x);
unsigned int hweight16(uint16_t x);
unsigned int hweight32(uint32_t x);
unsigned int hweight64(uint64_t x);

static inline unsigned int hweight_long(unsigned long x) {
    if (sizeof(x) == 8)
        return hweight64((uint64_t)x);
    else
        return hweight32((uint32_t)x);
}

void hweight_init(void);

#endif /* HWEIGHT_H */
