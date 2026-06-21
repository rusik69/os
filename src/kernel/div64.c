#include "div64.h"
#include "printf.h"
#include "kernel.h"

/*
 * 64-bit division support.
 *
 * In a freestanding environment without libgcc, the compiler may emit
 * calls to __udivdi3, __umoddi3, __divdi3, __moddi3.  This module
 * provides those helpers and validates the do_div macro.
 */

/* Provide the standard libgcc names for the linker */
uint64_t __udivdi3(uint64_t a, uint64_t b)
{
    if (b == 0) return 0;
    return a / b;
}

uint64_t __umoddi3(uint64_t a, uint64_t b)
{
    if (b == 0) return a;
    return a % b;
}

int64_t __divdi3(int64_t a, int64_t b)
{
    if (b == 0) return 0;
    return a / b;
}

int64_t __moddi3(int64_t a, int64_t b)
{
    if (b == 0) return a;
    return a % b;
}

uint64_t __udivmoddi4(uint64_t a, uint64_t b, uint64_t *rem)
{
    if (b == 0) {
        if (rem) *rem = a;
        return 0;
    }
    if (rem) *rem = a % b;
    return a / b;
}

void div64_init(void)
{
    /* Validate do_div macro */
    uint64_t test = 1000000ULL;
    uint32_t rem = do_div(test, 1000);
    (void)rem;

    kprintf("[OK] div64: 64-bit division support initialised\n");
}

/* ── Stub: div64_u64_rem ─────────────────────────────── */
uint64_t div64_u64_rem(uint64_t dividend, uint64_t divisor, uint64_t *remainder)
{
    (void)dividend;
    (void)divisor;
    (void)remainder;
    kprintf("[div64] div64_u64_rem: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: div64_u64 ─────────────────────────────── */
uint64_t div64_u64(uint64_t dividend, uint64_t divisor)
{
    (void)dividend;
    (void)divisor;
    kprintf("[div64] div64_u64: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: div_s64_rem ─────────────────────────────── */
int64_t div_s64_rem(int64_t dividend, int32_t divisor, int32_t *remainder)
{
    (void)dividend;
    (void)divisor;
    (void)remainder;
    kprintf("[div64] div_s64_rem: not yet implemented\n");
    return -ENOSYS;
}
