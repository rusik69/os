#include "sched_attr.h"
#include "process.h"
#include "scheduler.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "kernel.h"
#include "sched_deadline.h"

/*
 * Per-pid scheduling attributes table.
 * Indexed by (pid % SCHED_ATTR_TABLE_SIZE) for fast look-up.
 * In a production kernel these would live inside struct process.
 */
#define SCHED_ATTR_TABLE_SIZE  32

static struct sched_attr sched_attr_table[SCHED_ATTR_TABLE_SIZE];
static int               sched_attr_used[SCHED_ATTR_TABLE_SIZE];  /* 1 = slot occupied */
static int               sched_attr_initialised;

void sched_attr_init(void)
{
    if (sched_attr_initialised)
        return;

    memset(sched_attr_table, 0, sizeof(sched_attr_table));
    memset(sched_attr_used, 0, sizeof(sched_attr_used));
    sched_attr_initialised = 1;

    kprintf("[OK] sched_attr: scheduling attributes initialised\n");
}

static int sched_attr_index(uint32_t pid)
{
    return pid % SCHED_ATTR_TABLE_SIZE;
}

int sched_setattr(uint32_t pid, const struct sched_attr *attr, uint32_t flags)
{
    (void)flags;

    if (!sched_attr_initialised)
        return -ENOSYS;
    if (!attr)
        return -EFAULT;

    /* Validate size field */
    if (attr->size == 0 || attr->size > sizeof(struct sched_attr))
        return -EINVAL;

    /* Validate scheduling policy */
    if (attr->sched_policy < SCHED_OTHER || attr->sched_policy > SCHED_IDLE)
        return -EINVAL;

    /* Validate priority ranges */
    if (attr->sched_priority > 99)
        return -EINVAL;

    /* Validate nice range */
    if (attr->sched_nice < -20 || attr->sched_nice > 19)
        return -EINVAL;

    /* Validate deadline parameters */
    if (attr->sched_policy == SCHED_DEADLINE) {
        if (attr->sched_runtime == 0 || attr->sched_deadline == 0 || attr->sched_period == 0)
            return -EINVAL;
        if (attr->sched_runtime > attr->sched_deadline)
            return -EINVAL;
        if (attr->sched_deadline > attr->sched_period)
            return -EINVAL;
    }

    /* Find the process (must exist) */
    struct process *proc = process_get_by_pid(pid);
    if (!proc)
        return -ESRCH;

    int idx = sched_attr_index(pid);

    /* Store the scheduling attributes */
    memcpy(&sched_attr_table[idx], attr, sizeof(struct sched_attr));
    sched_attr_used[idx] = 1;

    /* Also update the process's in-core scheduling policy/priority */
    if (attr->sched_policy == SCHED_FIFO || attr->sched_policy == SCHED_RR ||
        attr->sched_policy == SCHED_OTHER || attr->sched_policy == SCHED_BATCH) {
        proc->sched_policy = (uint8_t)attr->sched_policy;
        proc->priority     = (uint8_t)(attr->sched_priority & 0xFF);
    } else if (attr->sched_policy == SCHED_IDLE) {
        proc->sched_policy = SCHED_IDLE;
        proc->priority     = SCHED_LEVELS - 1; /* lowest priority level */
    } else if (attr->sched_policy == SCHED_DEADLINE) {
        /* Configure SCHED_DEADLINE parameters */
        proc->sched_policy  = SCHED_DEADLINE;
        proc->dl_runtime    = attr->sched_runtime;
        proc->dl_deadline   = attr->sched_deadline;
        proc->dl_period     = attr->sched_period;
        proc->dl_active     = 0;
        proc->dl_throttled  = 0;

        /* Register with the deadline scheduler (admission control) */
        int ret = sched_deadline_add_task(proc);
        if (ret != 0) {
            kprintf("sched_deadline: admission control failed for pid %u "
                    "(runtime=%llu deadline=%llu period=%llu)\n",
                    pid,
                    (unsigned long long)attr->sched_runtime,
                    (unsigned long long)attr->sched_deadline,
                    (unsigned long long)attr->sched_period);
            return -EBUSY;
        }
    }

    return 0;
}

int sched_getattr(uint32_t pid, struct sched_attr *attr, size_t size, uint32_t flags)
{
    (void)flags;

    if (!sched_attr_initialised)
        return -ENOSYS;
    if (!attr)
        return -EFAULT;
    if (size > sizeof(struct sched_attr))
        return -EINVAL;

    /* Find the process (must exist) */
    struct process *proc = process_get_by_pid(pid);
    if (!proc)
        return -ESRCH;

    int idx = sched_attr_index(pid);

    if (!sched_attr_used[idx]) {
        /* No explicit sched_attr was stored — fill from the process struct */
        memset(attr, 0, size);
        attr->size           = sizeof(struct sched_attr);
        attr->sched_policy   = proc->sched_policy;
        attr->sched_flags    = 0;
        attr->sched_nice     = 0;
        attr->sched_priority = proc->priority;
        attr->sched_runtime  = 0;
        attr->sched_deadline = 0;
        attr->sched_period   = 0;
        return 0;
    }

    /* Copy from stored table */
    size_t copy_size = (size < sizeof(struct sched_attr)) ? size : sizeof(struct sched_attr);
    memcpy(attr, &sched_attr_table[idx], copy_size);
    if (size > sizeof(struct sched_attr))
        memset((uint8_t *)attr + sizeof(struct sched_attr), 0,
               size - sizeof(struct sched_attr));

    return 0;
}
