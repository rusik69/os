#ifndef SEQLOCK_H
#define SEQLOCK_H
#include "types.h"
typedef struct { volatile unsigned int seq; } seqlock_t;
#define DEFINE_SEQLOCK(x) seqlock_t x = { 0 }
static inline void seqlock_init(seqlock_t *sl) { sl->seq = 0; }
static inline unsigned int read_seqbegin(const seqlock_t *sl) {
    unsigned int ret;
    while (1) { ret = sl->seq; if (!(ret & 1)) break; __asm__ volatile("pause"); }
    __asm__ volatile("" : : : "memory");
    return ret;
}
static inline int read_seqretry(const seqlock_t *sl, unsigned int start) {
    __asm__ volatile("" : : : "memory");
    return sl->seq != start;
}
#endif
