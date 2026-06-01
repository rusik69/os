#ifndef BITOPS_H
#define BITOPS_H

#include "types.h"

static inline void set_bit(int bit, volatile void *addr) {
    __asm__ volatile(
        "lock btsq %1, %0"
        : "+m" (*(volatile unsigned long *)addr)
        : "Ir" ((unsigned long)bit)
        : "memory"
    );
}

static inline void clear_bit(int bit, volatile void *addr) {
    __asm__ volatile(
        "lock btrq %1, %0"
        : "+m" (*(volatile unsigned long *)addr)
        : "Ir" ((unsigned long)bit)
        : "memory"
    );
}

static inline int test_bit(int bit, const volatile void *addr) {
    unsigned long old;
    __asm__ volatile(
        "btq %2, %1; sbb %0, %0"
        : "=r" (old)
        : "m" (*(volatile unsigned long *)addr),
          "Ir" ((unsigned long)bit)
        : "memory"
    );
    return (int)old;
}

static inline int test_and_set_bit(int bit, volatile void *addr) {
    unsigned long old = 0;
    __asm__ volatile(
        "lock btsq %2, %1; sbb %0, %0"
        : "=r" (old), "+m" (*(volatile unsigned long *)addr)
        : "Ir" ((unsigned long)bit)
        : "memory"
    );
    return (int)(old != 0);
}

static inline int test_and_clear_bit(int bit, volatile void *addr) {
    unsigned long old = 0;
    __asm__ volatile(
        "lock btrq %2, %1; sbb %0, %0"
        : "=r" (old), "+m" (*(volatile unsigned long *)addr)
        : "Ir" ((unsigned long)bit)
        : "memory"
    );
    return (int)(old != 0);
}

/* 32-bit variants for compatibility */
static inline void set_bit32(int bit, volatile void *addr) {
    __asm__ volatile(
        "lock btsl %1, %0"
        : "+m" (*(volatile unsigned int *)addr)
        : "Ir" ((unsigned int)bit)
        : "memory"
    );
}

static inline void clear_bit32(int bit, volatile void *addr) {
    __asm__ volatile(
        "lock btrl %1, %0"
        : "+m" (*(volatile unsigned int *)addr)
        : "Ir" ((unsigned int)bit)
        : "memory"
    );
}

static inline int test_bit32(int bit, const volatile void *addr) {
    unsigned int old;
    __asm__ volatile(
        "btl %2, %1; sbb %0, %0"
        : "=r" (old)
        : "m" (*(volatile unsigned int *)addr),
          "Ir" ((unsigned int)bit)
        : "memory"
    );
    return (int)old;
}

#define BIT(nr) (1UL << (nr))

#endif /* BITOPS_H */
