#ifndef TIMECONST_H
#define TIMECONST_H

#include "types.h"

/*
 * Time constant definitions and helpers.
 *
 * Provides pre-computed time conversion constants and inline helpers
 * for converting between various time units without relying on
 * floating-point arithmetic.
 *
 * All constants are expressed with sufficient precision for kernel
 * timekeeping (nanosecond resolution).
 */

/* Hz to nanoseconds */
#define KHZ_TO_NSEC(hz)         (1000000000ULL / (uint64_t)(hz))

/* Standard frequency constants */
#define TIMECONST_HZ_100        100
#define TIMECONST_HZ_250        250
#define TIMECONST_HZ_300        300
#define TIMECONST_HZ_1000       1000

/* Pre-computed nanosecond per tick values */
#define NSEC_PER_SEC            1000000000ULL
#define NSEC_PER_MSEC           1000000ULL
#define NSEC_PER_USEC           1000ULL
#define USEC_PER_SEC            1000000ULL
#define MSEC_PER_SEC            1000ULL

/* Shift values for scaled math */
#define TIMECONST_SHIFT         32
#define TIMECONST_MULT          (1ULL << TIMECONST_SHIFT)

/*
 * timeconst_mult  - Compute the scaled multiplier for converting
 *                   ticks to nanoseconds:  mult = (NSEC_PER_SEC << shift) / Hz
 *                   Returns the fixed-point multiplier.
 */
static inline uint64_t timeconst_mult(uint32_t hz)
{
    return (NSEC_PER_SEC << TIMECONST_SHIFT) / hz;
}

/*
 * timeconst_ns_per_tick  - Return nanoseconds per tick for a given Hz.
 */
static inline uint64_t timeconst_ns_per_tick(uint32_t hz)
{
    return NSEC_PER_SEC / hz;
}

/*
 * timeconst_ticks_to_nsecs  - Convert @ticks to nanoseconds using
 *                             pre-computed @mult and @shift.
 */
static inline uint64_t timeconst_ticks_to_nsecs(uint64_t ticks,
                                                uint64_t mult,
                                                unsigned int shift)
{
    return (ticks * mult) >> shift;
}

/*
 * timeconst_init  - Initialise time constant subsystem.
 */
void timeconst_init(void);

#endif /* TIMECONST_H */
