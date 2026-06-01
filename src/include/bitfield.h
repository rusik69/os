#ifndef BITFIELD_H
#define BITFIELD_H

#include "types.h"

/* Bit-numbering macro */
#define BIT(n)            (1ULL << (n))

/* Generate a contiguous mask of bits from high to low (inclusive). */
#define GENMASK(h, l) \
    (((~0ULL) >> (63 - (h))) & ((~0ULL) << (l)))

/* Extract a field from 'value' defined by 'mask'.
 * Result is right-shifted so the least-significant bit of the field is bit 0. */
#define FIELD_GET(mask, value) \
    (typeof(mask))((((typeof(value))(value)) & (mask)) >> __builtin_ctzll(mask))

/* Prepare a value for placement at a field defined by 'mask'.
 * The caller provides 'value' already right-aligned (bit 0). */
#define FIELD_PREP(mask, value) \
    (((typeof(mask))(value) << __builtin_ctzll(mask)) & (mask))

/* Upper/lower bits of a 64-bit value */
#define U64_HIGH(x) ((uint32_t)((x) >> 32))
#define U64_LOW(x)  ((uint32_t)(x))

/* Set/clear/test a single bit in a word */
static inline void set_bit(int nr, volatile uint64_t *addr) {
    *addr |= BIT(nr);
}

static inline void clear_bit(int nr, volatile uint64_t *addr) {
    *addr &= ~BIT(nr);
}

static inline int test_bit(int nr, volatile const uint64_t *addr) {
    return !!(*addr & BIT(nr));
}

void bitfield_init(void);

#endif /* BITFIELD_H */
