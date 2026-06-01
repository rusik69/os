#ifndef MATH_EXT_H
#define MATH_EXT_H

#include "types.h"

/* ---- Absolute value ---- */
static inline int  abs(int  j) { return j < 0 ? -j : j; }
static inline long labs(long j) { return j < 0 ? -j : j; }
static inline long long llabs(long long j) { return j < 0 ? -j : j; }

/* ---- Division result types ---- */
typedef struct { int quot; int rem; }     div_t;
typedef struct { long quot; long rem; }   ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

/* ---- Division returning both quotient and remainder ---- */
static inline div_t   div(int numer, int denom) {
    div_t r = { numer / denom, numer % denom };
    return r;
}
static inline ldiv_t  ldiv(long numer, long denom) {
    ldiv_t r = { numer / denom, numer % denom };
    return r;
}
static inline lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t r = { numer / denom, numer % denom };
    return r;
}

/* ---- Min / max (type-generic using __typeof__) ---- */
#ifndef min
#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#endif
#ifndef max
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#endif

/* ---- Ceiling division for positive integers ---- */
#define ceil_div(a, b) (((a) + (b) - 1) / (b))

/* ---- Power-of-2 test ---- */
#define is_power_of_2(x) ((x) && !((x) & ((x) - 1)))

/* ---- Rounding to alignment (align must be power of 2) ---- */
#define round_up(x, align)   (((x) + (__typeof__(x))(align) - 1) & ~((__typeof__(x))(align) - 1))
#define round_down(x, align) ((x) & ~((__typeof__(x))(align) - 1))

#endif /* MATH_EXT_H */
