#ifndef PROCESS_RLIMIT_H
#define PROCESS_RLIMIT_H

#include "types.h"
#include "syscall.h"  /* For Linux ABI RLIMIT_* resource identifiers */

/*
 * Resource limits (rlimit) — per-process resource consumption limits.
 * Mirrors the Linux getrlimit/setrlimit/prlimit64 syscall interface.
 *
 * All RLIMIT_* values match the Linux ABI (from syscall.h) so that
 * enforcement code and userspace values use the same array indices.
 */

/* Extra internal resource limit(s) not in the Linux ABI */
#ifndef RLIMIT_RSS
#define RLIMIT_RSS      14  /* max resident set size (bytes) — kernel-internal, outside ABI range */
#endif

#ifndef RLIMIT_NLIMITS
#define RLIMIT_NLIMITS  15  /* number of resource types (must match _RLIMIT_NLIMITS in process.h) */
#endif

/* "Infinity" value for resources (max representable) */
#ifndef RLIM_INFINITY
#define RLIM_INFINITY   ((uint64_t)-1)
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
