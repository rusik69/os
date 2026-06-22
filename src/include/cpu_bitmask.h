#ifndef CPU_BITMASK_H
#define CPU_BITMASK_H

#include "types.h"

#define CPUMASK_MAX_CPUS 64

struct cpumask {
    uint64_t bits;
};

/* Operations on a single cpumask. */
static inline void cpumask_set_cpu(int cpu, struct cpumask *mask) {
    mask->bits |= (1ULL << cpu);
}

static inline void cpumask_clear_cpu(int cpu, struct cpumask *mask) {
    mask->bits &= ~(1ULL << cpu);
}

static inline int cpumask_test_cpu(int cpu, const struct cpumask *mask) {
    return !!(mask->bits & (1ULL << cpu));
}

static inline void cpumask_zero(struct cpumask *mask) {
    mask->bits = 0;
}

static inline void cpumask_fill(struct cpumask *mask) {
    mask->bits = ~0ULL;
}

static inline void cpumask_and(struct cpumask *dst,
                                const struct cpumask *a,
                                const struct cpumask *b) {
    dst->bits = a->bits & b->bits;
}

static inline void cpumask_or(struct cpumask *dst,
                               const struct cpumask *a,
                               const struct cpumask *b) {
    dst->bits = a->bits | b->bits;
}

static inline void cpumask_xor(struct cpumask *dst,
                                const struct cpumask *a,
                                const struct cpumask *b) {
    dst->bits = a->bits ^ b->bits;
}

static inline void cpumask_complement(struct cpumask *dst,
                                       const struct cpumask *src) {
    dst->bits = ~src->bits;
}

static inline int cpumask_empty(const struct cpumask *mask) {
    return mask->bits == 0;
}

static inline int cpumask_equal(const struct cpumask *a,
                                 const struct cpumask *b) {
    return a->bits == b->bits;
}

static inline int cpumask_full(const struct cpumask *mask) {
    return mask->bits == ~0ULL;
}

static inline int cpumask_weight(const struct cpumask *mask) {
    return __builtin_popcountll(mask->bits);
}

static inline int cpumask_first(const struct cpumask *mask) {
    if (mask->bits == 0) return CPUMASK_MAX_CPUS;
    return __builtin_ctzll(mask->bits);
}

static inline int cpumask_next(int cpu, const struct cpumask *mask) {
    uint64_t tmp = mask->bits & ~((1ULL << (cpu + 1)) - 1);
    if (tmp == 0) return CPUMASK_MAX_CPUS;
    return __builtin_ctzll(tmp);
}

static inline int cpumask_any(const struct cpumask *mask) {
    return cpumask_first(mask);
}

static inline int cpumask_any_but(const struct cpumask *mask, int cpu) {
    struct cpumask tmp;
    tmp.bits = mask->bits;               /* copy mask */
    cpumask_clear_cpu(cpu, &tmp);        /* remove the excluded cpu */
    return cpumask_first(&tmp);          /* first CPU still in mask */
}

/* Convert to/from unsigned long pointer (for system call compatibility). */
static inline void cpumask_copy_to_ulong(unsigned long *dst,
                                          const struct cpumask *src) {
    *dst = (unsigned long)src->bits;
}

void cpu_bitmask_init(void);

#endif /* CPU_BITMASK_H */
