#ifndef SEQLOCK_H
#define SEQLOCK_H
#include "types.h"
typedef struct { volatile unsigned int seq; } seqlock_t;
#define DEFINE_SEQLOCK(x) seqlock_t x = { 0 }
static inline void seqlock_init(seqlock_t *sl) { sl->seq = 0; }

/* ── Reader-side ──────────────────────────────────────────────────────── */

static inline unsigned int read_seqbegin(const seqlock_t *sl) {
    unsigned int ret;
    while (1) {
        ret = sl->seq;
        if (!(ret & 1)) break;
        __asm__ volatile("pause");
    }
    /* Compiler barrier: seq load must complete before data loads */
    __asm__ volatile("" : : : "memory");
    return ret;
}
static inline int read_seqretry(const seqlock_t *sl, unsigned int start) {
    /* Compiler barrier: data loads must complete before seq check */
    __asm__ volatile("" : : : "memory");
    return sl->seq != start;
}

/* ── Writer-side ────────────────────────────────────────────────────────
 *
 * NOTE: These functions do NOT provide mutual exclusion among writers.
 * Writers must be serialized externally (e.g., by the caller's context
 * such as a single timer tick, or via an external spinlock).  The seqlock
 * only coordinates readers with writers — it signals "write in progress"
 * to readers via an odd sequence number.
 *
 * On x86-64 Total Store Order (TSO), stores are never reordered with
 * other stores for write-back memory, so a compiler barrier suffices
 * for store-store ordering.  No SFENCE/MFENCE is required.
 */

static inline void write_seqlock(seqlock_t *sl) {
    /* Increment seq to odd — signals "writer active" to readers */
    sl->seq++;
    /* Compiler barrier: seq store must be visible before data stores */
    __asm__ volatile("" : : : "memory");
}
static inline void write_sequnlock(seqlock_t *sl) {
    /* Compiler barrier: data stores must complete before seq update */
    __asm__ volatile("" : : : "memory");
    /* Increment seq to even — signals "write complete" to readers */
    sl->seq++;
}
#endif
