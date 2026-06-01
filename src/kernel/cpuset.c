#include "cpuset.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "kernel.h"

/*
 * Per-process CPU affinity table.
 * Indexed by (pid % CPUSET_TABLE_SIZE).
 */
#define CPUSET_TABLE_SIZE  64

static cpuset_t cpuset_table[CPUSET_TABLE_SIZE];
static int      cpuset_used[CPUSET_TABLE_SIZE];
static int      cpuset_initialised;

/* Global cpuset representing all available CPUs */
static cpuset_t cpuset_all;

void cpuset_init(void)
{
    if (cpuset_initialised)
        return;

    memset(cpuset_table, 0, sizeof(cpuset_table));
    memset(cpuset_used, 0, sizeof(cpuset_used));

    /* Set all CPUs in the global "all" set */
#if CPUSET_MAX_CPUS >= 64
    cpuset_all.bits = ~0ULL;
#else
    cpuset_all.bits = (1ULL << CPUSET_MAX_CPUS) - 1;
#endif

    cpuset_initialised = 1;

    kprintf("[OK] cpuset: CPU affinity initialised (%d CPUs max)\n", CPUSET_MAX_CPUS);
}

/* Return a reference to the "all CPUs" cpuset */
const cpuset_t *cpuset_get_all(void)
{
    return &cpuset_all;
}

static int cpuset_index(uint32_t pid)
{
    return pid % CPUSET_TABLE_SIZE;
}

int sched_setaffinity(uint32_t pid, const cpuset_t *cpuset)
{
    if (!cpuset_initialised)
        return -ENOSYS;
    if (!cpuset)
        return -EFAULT;

    /* Must set at least one CPU */
    if (cpuset_empty(cpuset))
        return -EINVAL;

    /* Validate no bits beyond max CPUs */
    if (CPUSET_MAX_CPUS < 64 && (cpuset->bits & ~((1ULL << CPUSET_MAX_CPUS) - 1)))
        return -EINVAL;

    struct process *proc = process_get_by_pid(pid);
    if (!proc)
        return -ESRCH;

    int idx = cpuset_index(pid);

    cpuset_table[idx] = *cpuset;
    cpuset_used[idx] = 1;

    /* Store a compact representation in the process struct */
    proc->cpu_affinity = (uint8_t)(cpuset->bits & 0xFF);

    return 0;
}

int sched_getaffinity(uint32_t pid, cpuset_t *cpuset)
{
    if (!cpuset_initialised)
        return -ENOSYS;
    if (!cpuset)
        return -EFAULT;

    struct process *proc = process_get_by_pid(pid);
    if (!proc)
        return -ESRCH;

    int idx = cpuset_index(pid);

    if (cpuset_used[idx]) {
        *cpuset = cpuset_table[idx];
    } else {
        /* Default: all CPUs */
        *cpuset = cpuset_all;
    }

    return 0;
}
