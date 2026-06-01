#ifndef ATOMIC_H
#define ATOMIC_H

#include "types.h"

typedef struct {
    volatile int32_t counter;
} atomic_t;

#define ATOMIC_INIT(i) { (i) }

static inline int atomic_read(atomic_t *v) {
    return v->counter;
}

static inline void atomic_set(atomic_t *v, int i) {
    v->counter = i;
}

static inline void atomic_add(atomic_t *v, int i) {
    __asm__ volatile(
        "lock xaddl %0, %1"
        : "+r" (i), "+m" (v->counter)
        :
        : "memory"
    );
}

static inline void atomic_sub(atomic_t *v, int i) {
    int neg = -i;
    __asm__ volatile(
        "lock xaddl %0, %1"
        : "+r" (neg), "+m" (v->counter)
        :
        : "memory"
    );
}

static inline int atomic_add_return(atomic_t *v, int i) {
    int __ret = i;
    __asm__ volatile(
        "lock xaddl %0, %1"
        : "+r" (__ret), "+m" (v->counter)
        :
        : "memory"
    );
    return __ret + i;
}

static inline int atomic_sub_return(atomic_t *v, int i) {
    return atomic_add_return(v, -i);
}

static inline void atomic_inc(atomic_t *v) {
    __asm__ volatile(
        "lock incl %0"
        : "+m" (v->counter)
        :
        : "memory"
    );
}

static inline void atomic_dec(atomic_t *v) {
    __asm__ volatile(
        "lock decl %0"
        : "+m" (v->counter)
        :
        : "memory"
    );
}

static inline int atomic_dec_and_test(atomic_t *v) {
    unsigned char c;
    __asm__ volatile(
        "lock decl %0; setz %1"
        : "+m" (v->counter), "=qm" (c)
        :
        : "memory"
    );
    return c != 0;
}

#endif /* ATOMIC_H */
