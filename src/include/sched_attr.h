#ifndef SCHED_ATTR_H
#define SCHED_ATTR_H

#include "types.h"

/* Scheduling attributes structure (mirrors Linux sched_setattr/getattr) */
struct sched_attr {
    size_t   size;             /* size of this structure */
    int      sched_policy;     /* SCHED_OTHER, SCHED_FIFO, SCHED_RR, SCHED_DEADLINE */
    uint64_t sched_flags;      /* SCHED_FLAG_* */
    int      sched_nice;       /* nice value (-20..19) */
    uint32_t sched_priority;   /* RT priority (1..99 for FIFO/RR) */
    uint64_t sched_runtime;    /* runtime for deadline scheduling (ns) */
    uint64_t sched_deadline;   /* deadline for deadline scheduling (ns) */
    uint64_t sched_period;     /* period for deadline scheduling (ns) */
};

/* sched_flags */
#define SCHED_FLAG_RESET_ON_FORK  1
#define SCHED_FLAG_RECLAIM        2
#define SCHED_FLAG_DL_OVERRUN     4

/* Syscall-like wrappers: note that these use a static table indexed by
 * pid % 32 for quick storage.  In a real kernel these would be stored
 * in the process descriptor. */
int sched_setattr(uint32_t pid, const struct sched_attr *attr, uint32_t flags);
int sched_getattr(uint32_t pid, struct sched_attr *attr, size_t size, uint32_t flags);

/* Init called during kernel boot */
void sched_attr_init(void);

#endif /* SCHED_ATTR_H */
