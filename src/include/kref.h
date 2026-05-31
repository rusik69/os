#ifndef KREF_H
#define KREF_H

#include "types.h"

struct kref {
    int count;
};

/* Initialize the reference counter to the given initial value. */
static inline void kref_init(struct kref *r, int initial) {
    r->count = initial;
}

/* Increment the reference count (no release callback here). */
static inline void kref_get(struct kref *r) {
    __sync_add_and_fetch(&r->count, 1);
}

/* Decrement the reference count. If it drops to 0, call the release function. */
static inline int kref_put(struct kref *r, void (*release)(struct kref *)) {
    if (__sync_sub_and_fetch(&r->count, 1) == 0) {
        if (release) release(r);
        return 1;
    }
    return 0;
}

#endif /* KREF_H */
