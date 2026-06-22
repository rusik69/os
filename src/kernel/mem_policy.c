#include "mem_policy.h"
#include "string.h"
#include "kernel.h"
#include "printf.h"
#include "errno.h"
#include "caps.h"

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

/* ═══════════════════════════════════════════════════════════════════════
 *  Stub functions for incomplete memory policy operations
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── mempolicy_set ────────────────────────────────────────────────────── */
/*
 * Set the memory policy for the current process.
 * Validates mode and applies the policy to the process's policy state.
 * Returns 0 on success, -EINVAL on invalid parameters.
 */
int mempolicy_set(int mode, uint64_t nodemask, int preferred_node)
{
    if (!mem_policy_initialised)
        mem_policy_init();

    /* Validate mode */
    switch (mode) {
    case MPOL_DEFAULT:
        current_policy.mode = MPOL_DEFAULT;
        current_policy.nodemask = 0;
        current_policy.preferred_node = 0;
        break;
    case MPOL_BIND:
        /* Must have at least one node in the mask */
        if (nodemask == 0)
            return -EINVAL;
        current_policy.mode = MPOL_BIND;
        current_policy.nodemask = nodemask;
        current_policy.preferred_node = 0;
        break;
    case MPOL_INTERLEAVE:
        if (nodemask == 0)
            return -EINVAL;
        current_policy.mode = MPOL_INTERLEAVE;
        current_policy.nodemask = nodemask;
        current_policy.interleave_slot = 0;
        break;
    case MPOL_PREFERRED:
        if (preferred_node < 0 || preferred_node >= MPOL_MAX_NODES)
            return -EINVAL;
        current_policy.mode = MPOL_PREFERRED;
        current_policy.nodemask = nodemask;
        current_policy.preferred_node = preferred_node;
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

/* ── mempolicy_get ────────────────────────────────────────────────────── */
/*
 * Get the current memory policy.
 * Returns 0 on success, -EINVAL if output pointers are NULL.
 */
int mempolicy_get(int *mode, uint64_t *nodemask, int *preferred_node)
{
    if (!mem_policy_initialised)
        mem_policy_init();

    if (!mode)
        return -EINVAL;

    *mode = current_policy.mode;

    if (nodemask)
        *nodemask = current_policy.nodemask;

    if (preferred_node)
        *preferred_node = current_policy.preferred_node;

    return 0;
}

/* ── mempolicy_mbind ──────────────────────────────────────────────────── */
/*
 * Apply a memory policy to a range of virtual addresses.
 * Validates parameters and updates the policy for the address range.
 * Returns 0 on success, -EINVAL on invalid parameters.
 */
int mempolicy_mbind(uint64_t addr, uint64_t len, int mode, uint64_t nodemask)
{
    if (!mem_policy_initialised)
        mem_policy_init();

    /* Validate address and length */
    if (addr + len < addr || len == 0)
        return -EINVAL;

    /* Validate mode */
    switch (mode) {
    case MPOL_DEFAULT:
    case MPOL_BIND:
    case MPOL_INTERLEAVE:
    case MPOL_PREFERRED:
        break;
    default:
        return -EINVAL;
    }

    /* For MPOL_BIND/MPOL_INTERLEAVE, nodemask must be non-zero */
    if ((mode == MPOL_BIND || mode == MPOL_INTERLEAVE) && nodemask == 0)
        return -EINVAL;

    /* Apply the policy to the address range.
     * In a full implementation we would walk the VMA tree and set
     * per-VMA policy structs. Here we store a single global policy
     * hint for demonstration purposes. */
    current_policy.mode = mode;
    current_policy.nodemask = nodemask;

    if (mode == MPOL_PREFERRED) {
        /* Find the first set bit in nodemask as preferred node */
        int pref = -1;
        for (int i = 0; i < MPOL_MAX_NODES; i++) {
            if (nodemask & (1ULL << i)) {
                pref = i;
                break;
            }
        }
        current_policy.preferred_node = (pref >= 0) ? pref : 0;
    }

    return 0;
}

/* ── mempolicy_migrate_pages ──────────────────────────────────────────── */
/*
 * Migrate pages of a process to new NUMA nodes.
 * @pid:           PID of the process whose pages to migrate.
 * @new_nodemask:  Bitmask of destination NUMA nodes.
 *
 * Returns 0 on success, -EINVAL for invalid PID, -EPERM if not permitted.
 */
int mempolicy_migrate_pages(int pid, uint64_t new_nodemask)
{
    if (!mem_policy_initialised)
        mem_policy_init();

    if (new_nodemask == 0)
        return -EINVAL;

    struct process *target = NULL;

    if (pid == 0) {
        /* Migrate current process's pages */
        target = process_get_current();
    } else {
        /* Find the target process */
        target = process_get_by_pid((uint32_t)pid);
    }

    if (!target)
        return -EINVAL;

    /* Check permissions: must be root, or have CAP_SYS_NICE
     * (or be the same process) */
    struct process *caller = process_get_current();
    if (!caller)
        return -EPERM;

    if (caller != target) {
        /* Check if caller has CAP_SYS_NICE */
        int word = CAP_SYS_NICE / 64;
        int bit  = CAP_SYS_NICE % 64;
        if (word >= PROCESS_SYSCALL_CAP_WORDS ||
            !(caller->syscall_caps[word] & (1ULL << bit))) {
            return -EPERM;
        }
    }

    /* Set the new nodemask as the process's memory policy */
    current_policy.nodemask = new_nodemask;
    current_policy.mode = MPOL_BIND;

    kprintf("[mem_policy] Migrating PID %d to nodemask 0x%llx\n",
            pid, (unsigned long long)new_nodemask);

    return 0;
}
