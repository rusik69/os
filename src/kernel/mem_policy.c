#include "mem_policy.h"
#include "string.h"
#include "kernel.h"
#include "printf.h"
#include "errno.h"

/* Per-process memory policy — in a real kernel this would live in the
 * task_struct.  For now we use a single global instance to represent the
 * current process. */
static struct mem_policy current_policy;
static int mem_policy_initialised = 0;

void mem_policy_init(void)
{
    if (mem_policy_initialised)
        return;

    current_policy.mode = MPOL_DEFAULT;
    current_policy.nodemask = 0;
    current_policy.preferred_node = 0;
    current_policy.interleave_slot = 0;

    mem_policy_initialised = 1;
    kprintf("mem_policy: initialised (default policy DEFAULT)\n");
}

int set_mempolicy(int mode, uint64_t nodemask, int preferred_node)
{
    (void)mode;
    (void)nodemask;
    (void)preferred_node;
    /* set_mempolicy_home_node: no-op stub */
    return 0;
}

int get_mempolicy(int *mode, uint64_t *nodemask, int *preferred_node)
{
    if (mode)
        *mode = current_policy.mode;
    if (nodemask)
        *nodemask = current_policy.nodemask;
    if (preferred_node)
        *preferred_node = current_policy.preferred_node;

    return 0;
}

int mbind(uint64_t addr, uint64_t len, int mode, uint64_t nodemask, int preferred_node)
{
    if (addr + len < addr || len == 0)
        return -EINVAL;

    /* Validate mode. */
    switch (mode) {
    case MPOL_DEFAULT:
    case MPOL_BIND:
    case MPOL_INTERLEAVE:
    case MPOL_PREFERRED:
        break;
    default:
        return -EINVAL;
    }

    /* Apply the policy to the given range.
     * In a full implementation each VMA in the range would get its own
     * policy struct.  Here we simply validate and record a global hint. */
    (void)addr;
    (void)len;
    (void)nodemask;
    (void)preferred_node;

    return 0;
}
