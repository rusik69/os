#ifndef REFCOUNT_H
#define REFCOUNT_H

#include "types.h"
#include "atomic.h"

#define REFCOUNT_SATURATED (INT32_MAX / 2)
#define REFCOUNT_MAX       (INT32_MAX / 2)

typedef struct {
    atomic_t refs;
} refcount_t;

#define REFCOUNT_INIT(n) { ATOMIC_INIT(n) }

static inline void refcount_set(refcount_t *r, int n) {
    atomic_set(&r->refs, n);
}

static inline int refcount_read(refcount_t *r) {
    return atomic_read(&r->refs);
}

static inline int refcount_get(refcount_t *r) {
    int old = atomic_add_return(&r->refs, 1);
    if (old > REFCOUNT_SATURATED) {
        atomic_set(&r->refs, REFCOUNT_SATURATED);
        return REFCOUNT_SATURATED;
    }
    return old;
}

static inline int refcount_put(refcount_t *r) {
    int old = atomic_sub_return(&r->refs, 1);
    if (old == 0) {
        return 1; /* caller should free */
    }
    if (old < 0) {
        /* Underflow detected; saturate to 0 */
        atomic_set(&r->refs, 0);
        return 1;
    }
    return 0;
}

#endif /* REFCOUNT_H */
