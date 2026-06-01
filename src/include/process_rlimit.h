#ifndef PROCESS_RLIMIT_H
#define PROCESS_RLIMIT_H

#include "types.h"

/*
 * Resource limits (rlimit) — per-process resource consumption limits.
 * Mirrors the Linux getrlimit/setrlimit/prlimit64 syscall interface.
 */

/* Resource identifiers */
#ifndef RLIMIT_CPU
#define RLIMIT_CPU      0   /* CPU time (seconds) */
#endif
#ifndef RLIMIT_FSIZE
#define RLIMIT_FSIZE    1   /* max file size (bytes) */
#endif
#ifndef RLIMIT_DATA
#define RLIMIT_DATA     2   /* max data segment size (bytes) */
#endif
#ifndef RLIMIT_STACK
#define RLIMIT_STACK    3   /* max stack size (bytes) */
#endif
#ifndef RLIMIT_CORE
#define RLIMIT_CORE     4   /* max core file size (bytes) */
#endif
#ifndef RLIMIT_RSS
#define RLIMIT_RSS      5   /* max resident set size (bytes) */
#endif
#ifndef RLIMIT_NPROC
#define RLIMIT_NPROC    6   /* max number of processes */
#endif
#ifndef RLIMIT_NOFILE
#define RLIMIT_NOFILE   7   /* max number of open files */
#endif
#ifndef RLIMIT_MEMLOCK
#define RLIMIT_MEMLOCK  8   /* max locked-in-memory size (bytes) */
#endif
#ifndef RLIMIT_AS
#define RLIMIT_AS       9   /* address space limit (bytes) */
#endif
#ifndef RLIMIT_NLIMITS
#define RLIMIT_NLIMITS  10  /* number of resource types */
#endif

/* "Infinity" value for resources (max representable) */
#ifndef RLIM_INFINITY
#define RLIM_INFINITY  ((uint64_t)-1)
#endif

/* rlimit structure used by getrlimit/setrlimit */
struct rlimit {
    uint64_t rlim_cur;    /* soft limit (current enforced limit) */
    uint64_t rlim_max;    /* hard limit (ceiling for soft limit) */
};

/* ── API ──────────────────────────────────────────────────────── */

/* Get resource limits for a process */
int rlimit_get(uint32_t pid, int resource, struct rlimit *rlim);

/* Set resource limits for a process (subject to privilege) */
int rlimit_set(uint32_t pid, int resource, const struct rlimit *rlim);

/* Check whether a proposed value falls within the soft limit.
 * Returns 0 if OK, -EPERM if exceeded. */
int rlimit_check(uint32_t pid, int resource, uint64_t value);

/* Init called during kernel boot — sets all limits to RLIM_INFINITY */
void rlimit_init(void);

#endif /* PROCESS_RLIMIT_H */
