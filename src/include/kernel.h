#ifndef KERNEL_H
#define KERNEL_H
#include "types.h"
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define container_of(ptr, type, member) ({ const __typeof__(((type *)0)->member) *__mptr = (ptr); (type *)((char *)__mptr - offsetof(type, member)); })
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define min(x, y) ({ __typeof__(x) _x = (x); __typeof__(y) _y = (y); _x < _y ? _x : _y; })
#define max(x, y) ({ __typeof__(x) _x = (x); __typeof__(y) _y = (y); _x > _y ? _x : _y; })
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define abs(x) ({ __typeof__(x) _x = (x); _x < 0 ? -_x : _x; })
#define __maybe_unused __attribute__((unused))
#define barrier() __asm__ volatile("" : : : "memory")
#define mb()      __asm__ volatile("mfence" : : : "memory")
#define rmb()     __asm__ volatile("lfence" : : : "memory")
#define wmb()     __asm__ volatile("sfence" : : : "memory")

/*
 * READ_ONCE / WRITE_ONCE — prevent compiler from merging, tearing, or
 * caching accesses to shared variables.  On x86-64, aligned loads and
 * stores are naturally atomic; these macros add the volatile qualifier
 * so the compiler emits exactly one load/store and re-fetches on every
 * use.  Use for any global variable that is read or written in
 * interrupt context without holding a spinlock that includes a
 * compiler barrier.
 */
#define READ_ONCE(x)                    (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, val)             ((*(volatile __typeof__(x) *)&(x)) = (val))
#endif
