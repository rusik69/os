#ifndef MEM_POLICY_H
#define MEM_POLICY_H

#include "types.h"

#define MPOL_DEFAULT    0
#define MPOL_BIND       1
#define MPOL_INTERLEAVE 2
#define MPOL_PREFERRED  3

#define MPOL_MAX_NODES 64

struct mem_policy {
    int mode;                        /* MPOL_DEFAULT/BIND/INTERLEAVE/PREFERRED */
    uint64_t nodemask;               /* bitmask of allowed NUMA nodes */
    int preferred_node;              /* preferred node for MPOL_PREFERRED */
    int interleave_slot;             /* next interleave slot (round-robin index) */
};

/* Set the memory policy for the current process.  Returns 0 on success, -errno on error. */
int set_mempolicy(int mode, uint64_t nodemask, int preferred_node);

/* Get the current memory policy. */
int get_mempolicy(int *mode, uint64_t *nodemask, int *preferred_node);

/* Bind memory pages to a specific policy. */
int mbind(uint64_t addr, uint64_t len, int mode, uint64_t nodemask, int preferred_node);

/* Initialise the memory policy subsystem. */
void mem_policy_init(void);

/* Set/Get memory policy (richer interface used by syscalls). */
int mempolicy_set(int mode, uint64_t nodemask, int preferred_node);
int mempolicy_get(int *mode, uint64_t *nodemask, int *preferred_node);
int mempolicy_mbind(uint64_t addr, uint64_t len, int mode, uint64_t nodemask);
int mempolicy_migrate_pages(int pid, uint64_t new_nodemask);

/* Flags for sys_move_pages */
#define MPOL_MF_MOVE        (1U << 0)  /* move pages */
#define MPOL_MF_MOVE_ALL    (1U << 1)  /* move all pages */

#endif /* MEM_POLICY_H */
