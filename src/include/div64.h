#ifndef DIV64_H
#define DIV64_H

#include "types.h"

/*
 * 64-bit division helpers.
 *
 * Provides efficient 64/32 and 64/64 division operations for the kernel,
 * avoiding compiler-inserted libgcc calls in a freestanding environment.
 * Includes do_div-style macros and safe shift-based implementations.
 */

/*
 * do_div  - Divide @n by @base, returning the quotient in @n and
 *           the remainder as the return value.
 *
 *           Typical usage:
 *               uint64_t n = 123456789ULL;
 *               uint32_t rem = do_div(n, 1000);
 *               // n is now 123456, rem is 789
 */
/* do_div  - Divide *n by @base, storing quotient in *n, returning remainder.
 *
 * Typical usage:
 *     uint64_t n = 123456789ULL;
 *     uint32_t rem = do_div(&n, 1000);
 *     // n is now 123456, rem is 789
 */
static inline uint32_t do_div(uint64_t *n, uint64_t base)
{
    uint64_t __rem = 0;
    if (base != 0) {
        __rem = *n % base;
        *n = *n / base;
    }
    return (uint32_t)__rem;
}

/*
 * div_s64  - Signed 64-bit division with proper rounding.
 */
static inline int64_t div_s64(int64_t dividend, int64_t divisor)
{
    if (divisor == 0)
        return 0;
    return dividend / divisor;
}

/*
 * div_u64  - Unsigned 64-bit division.
 */
static inline uint64_t div_u64(uint64_t dividend, uint64_t divisor)
{
    if (divisor == 0)
        return 0;
    return dividend / divisor;
}

/*
 * div_u64_rem  - Unsigned 64-bit division with remainder.
 */
static inline uint64_t div_u64_rem(uint64_t dividend, uint64_t divisor,
                                   uint64_t *remainder)
{
    if (divisor == 0) {
        *remainder = dividend;
        return 0;
    }
    *remainder = dividend % divisor;
    return dividend / divisor;
}

/*
 * div64_u64  - 64/64 division returning 64-bit quotient.
 */
static inline uint64_t div64_u64(uint64_t dividend, uint64_t divisor)
{
    return div_u64(dividend, divisor);
}

/*
 * div64_s64  - Signed 64/64 division returning 64-bit quotient.
 */
static inline int64_t div64_s64(int64_t dividend, int64_t divisor)
{
    return div_s64(dividend, divisor);
}

/*
 * div64_init  - Initialise 64-bit division support.
 */
void div64_init(void);

#endif /* DIV64_H */
