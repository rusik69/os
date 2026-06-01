#ifndef KERNEL_H
#define KERNEL_H
#include "types.h"
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define container_of(ptr, type, member) ({ const typeof(((type *)0)->member) *__mptr = (ptr); (type *)((char *)__mptr - offsetof(type, member)); })
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((typeof(x))(a) - 1))
#define min(x, y) ({ typeof(x) _x = (x); typeof(y) _y = (y); _x < _y ? _x : _y; })
#define max(x, y) ({ typeof(x) _x = (x); typeof(y) _y = (y); _x > _y ? _x : _y; })
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define abs(x) ({ typeof(x) _x = (x); _x < 0 ? -_x : _x; })
#define barrier() __asm__ volatile("" : : : "memory")
#define mb()      __asm__ volatile("mfence" : : : "memory")
#define rmb()     __asm__ volatile("lfence" : : : "memory")
#define wmb()     __asm__ volatile("sfence" : : : "memory")
#endif
