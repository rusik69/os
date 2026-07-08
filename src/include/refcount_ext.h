#ifndef REFCOUNT_EXT_H
#define REFCOUNT_EXT_H

#include "types.h"
#include "atomic.h"

/*
 * Extended reference-count type with saturation and overflow/underflow
 * detection.
 *
 * Based on Linux refcount_t.  The counter is an atomic_t; operations
 * WARN (via kprintf) on overflow/underflow and saturate the counter
 * so the bug is not silently exploitable.
 */

struct refcount_struct {
    atomic_t refs;
};

#define REFCOUNT_INIT(n)  { .refs = ATOMIC_INIT(n) }

/*
 * refcount_set  - Set reference count to a known value.
 * Only safe when the caller already holds a reference.
 */
static inline void refcount_set(struct refcount_struct *r, int n)
{
    atomic_set(&r->refs, n);
}

/*
 * refcount_read  - Return the current reference count value.
 */
static inline int refcount_read(const struct refcount_struct *r)
{
    return atomic_read((atomic_t *)(uintptr_t)&r->refs);
}

/*
 * refcount_inc  - Increment with overflow detection.
 * WARNs and saturates to INT_MAX/2 on overflow.
 */
void refcount_inc(struct refcount_struct *r);

/*
 * refcount_dec  - Decrement with underflow detection.
 * WARNs and saturates to INT_MAX/2 on underflow.
 */
void refcount_dec(struct refcount_struct *r);

/*
 * refcount_inc_not_zero  - Increment unless the count is zero.
 * Returns 1 on success, 0 if the count was already zero.
 */
int refcount_inc_not_zero(struct refcount_struct *r);

/*
 * refcount_sub_and_test  - Subtract 'i' and test if the result is zero.
 * Returns 1 if the count reached zero, 0 otherwise.
 * WARNs on underflow.
 */
int refcount_sub_and_test(struct refcount_struct *r, int i);

/*
 * refcount_dec_and_test  - Decrement and test for zero.
 * Convenience wrapper around refcount_sub_and_test.
 */
static inline int refcount_dec_and_test(struct refcount_struct *r)
{
    return refcount_sub_and_test(r, 1);
}

void refcount_ext_init(void);

#endif /* REFCOUNT_EXT_H */
