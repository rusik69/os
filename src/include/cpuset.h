#ifndef CPUSET_H
#define CPUSET_H

#include "types.h"

#define CPUSET_MAX_CPUS  64

typedef struct {
    uint64_t bits;          /* bitmask of CPUs (up to 64 CPUs) */
} cpuset_t;

/* Single-CPU operations */
static inline void cpuset_zero(cpuset_t *set)
{
    set->bits = 0;
}

static inline void cpuset_set_cpu(int cpu, cpuset_t *set)
{
    if (cpu >= 0 && cpu < CPUSET_MAX_CPUS)
        set->bits |= (1ULL << cpu);
}

static inline void cpuset_clr_cpu(int cpu, cpuset_t *set)
{
    if (cpu >= 0 && cpu < CPUSET_MAX_CPUS)
        set->bits &= ~(1ULL << cpu);
}

static inline int cpuset_isset(int cpu, const cpuset_t *set)
{
    if (cpu < 0 || cpu >= CPUSET_MAX_CPUS)
        return 0;
    return (set->bits >> cpu) & 1ULL;
}

/* Set-wise operations */
static inline void cpuset_and(cpuset_t *dest, const cpuset_t *a, const cpuset_t *b)
{
    dest->bits = a->bits & b->bits;
}

static inline void cpuset_or(cpuset_t *dest, const cpuset_t *a, const cpuset_t *b)
{
    dest->bits = a->bits | b->bits;
}

static inline int cpuset_empty(const cpuset_t *set)
{
    return set->bits == 0;
}

static inline int cpuset_weight(const cpuset_t *set)
{
    return __builtin_popcountll(set->bits);
}

/* Syscall-like wrappers */
int sched_setaffinity(uint32_t pid, const cpuset_t *cpuset);
int sched_getaffinity(uint32_t pid, cpuset_t *cpuset);

/* Init called during kernel boot */
void cpuset_init(void);

#endif /* CPUSET_H */
