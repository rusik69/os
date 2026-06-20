#include "process_rlimit.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "kernel.h"

/*
 * Resource limits (rlimit) implementation.
 *
 * The actual per-process rlimit values are stored inside struct process
 * (rlim_cur[] and rlim_max[] arrays, defined in process.h).
 *
 * This module provides the get/set/check API and initialisation.
 */

static int rlimit_initialised;

void rlimit_init(void)
{
    if (rlimit_initialised)
        return;

    /* Initialise default limits for all existing processes.
     * rlim_cur and rlim_max are zero-initialised on boot, so we
     * set them to RLIM_INFINITY (unlimited).
     *
     * In this kernel we iterate through all process table slots and
     * apply defaults. */
    struct process *table = process_get_table();
    if (table) {
        for (int i = 0; i < PROCESS_MAX; i++) {
            for (int r = 0; r < RLIMIT_NLIMITS; r++) {
                table[i].rlim_cur[r] = RLIM_INFINITY;
                table[i].rlim_max[r] = RLIM_INFINITY;
            }
            /* Set sensible defaults for a few resources */
            table[i].rlim_cur[RLIMIT_NOFILE] = 1024;
            table[i].rlim_max[RLIMIT_NOFILE] = 4096;
            table[i].rlim_cur[RLIMIT_NPROC]  = 4096;
            table[i].rlim_max[RLIMIT_NPROC]  = 4096;
        }
    }

    rlimit_initialised = 1;

    kprintf("[OK] process_rlimit: resource limits initialised (infinity defaults)\n");
}

static int rlim_valid_resource(int resource)
{
    return resource >= 0 && resource < RLIMIT_NLIMITS;
}

int rlimit_get(uint32_t pid, int resource, struct rlimit *rlim)
{
    if (!rlim_valid_resource(resource))
        return -EINVAL;
    if (!rlim)
        return -EFAULT;

    struct process *proc = process_get_by_pid(pid);
    if (!proc)
        return -ESRCH;

    rlim->rlim_cur = proc->rlim_cur[resource];
    rlim->rlim_max = proc->rlim_max[resource];

    return 0;
}

int rlimit_set(uint32_t pid, int resource, const struct rlimit *rlim)
{
    if (!rlim_valid_resource(resource))
        return -EINVAL;
    if (!rlim)
        return -EFAULT;

    struct process *proc = process_get_by_pid(pid);
    if (!proc)
        return -ESRCH;

    /* Validate: new soft limit must not exceed the existing hard limit */
    if (rlim->rlim_cur > proc->rlim_max[resource])
        return -EPERM;

    /* Validate: new hard limit must not exceed current hard limit
     * (unless caller has CAP_SYS_RESOURCE, which we skip for simplicity) */
    if (rlim->rlim_max > proc->rlim_max[resource])
        return -EPERM;

    proc->rlim_cur[resource] = rlim->rlim_cur;
    proc->rlim_max[resource] = rlim->rlim_max;

    /* Update fd limit shorthand if NOFILE changed */
    if (resource == RLIMIT_NOFILE)
        proc->file_max = rlim->rlim_cur;

    return 0;
}

int rlimit_check(uint32_t pid, int resource, uint64_t value)
{
    if (!rlimit_initialised)
        return 0;    /* if not init'd, allow everything */
    if (!rlim_valid_resource(resource))
        return -EINVAL;

    struct process *proc = process_get_by_pid(pid);
    if (!proc)
        return -ESRCH;

    /* If the soft limit is RLIM_INFINITY, everything is allowed */
    if (proc->rlim_cur[resource] == RLIM_INFINITY)
        return 0;

    if (value > proc->rlim_cur[resource])
        return -EPERM;   /* would exceed resource limit */

    return 0;
}
