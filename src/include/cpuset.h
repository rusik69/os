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

/* ── CPU cgroup controller (Item 40) ────────────────────────────────
 *
 * Each CPU cgroup tracks accumulated CPU usage ticks and an optional
 * limit.  When a group exceeds its per-interval limit, all member
 * processes are temporarily throttled (skipped by the scheduler)
 * until the next accounting window.
 *
 * Group 0 is the default (unlimited, always runnable).
 */

#define CPU_CGROUP_MAX 16

struct cpu_cgroup {
    int      in_use;              /* 1 = this slot is active */
    int      cg_id;               /* cgroup ID (1..CPU_CGROUP_MAX-1) */
    uint64_t usage_ticks;         /* CPU ticks consumed in current window */
    uint64_t limit_ticks;         /* max ticks per window (0 = unlimited) */
    int      throttled;           /* 1 = group is currently throttled */
    int      member_count;        /* number of processes in this group */
};

/* Accessor for the cgroup table (defined in cpuset.c) */
struct cpu_cgroup *cpu_cgroup_get(int cg_id);
int  cpu_cgroup_alloc(void);               /* allocate a new cgroup, returns ID */
int  cpu_cgroup_set_limit(int cg_id, uint64_t limit_ticks);
void cpu_cgroup_account(int cg_id, uint64_t ticks);
int  cpu_cgroup_is_throttled(int cg_id);
void cpu_cgroup_reset_window(void);        /* reset all usage counters (called periodically) */

#endif /* CPUSET_H */
