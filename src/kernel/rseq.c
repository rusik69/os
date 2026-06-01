#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "vmm.h"
#include "errno.h"
#include "rseq.h"

/* Track rseq registration in the process structure.
 * We extend the process struct via an auxiliary structure since
 * we cannot modify process.h without touching many other files. */

/* Auxiliary per-process rseq data */
struct rseq_state {
    uint64_t rseq_addr;     /* user address of struct rseq */
    uint32_t rseq_len;      /* size of rseq structure */
    uint32_t rseq_sig;      /* signature for abort handler */
    int      registered;
};

/* Since we can't easily add fields to struct process without modifying process.h,
 * we maintain a parallel table indexed by PID. */
static struct rseq_state rseq_table[PROCESS_MAX];
static int rseq_initialized = 0;

void rseq_init(void)
{
    memset(rseq_table, 0, sizeof(rseq_table));
    rseq_initialized = 1;
    kprintf("[OK] rseq: restartable sequences initialized\n");
}

static struct rseq_state *rseq_get_state(struct process *proc)
{
    if (!proc || proc->pid >= PROCESS_MAX)
        return NULL;
    return &rseq_table[proc->pid];
}

int rseq_register(struct process *proc, uint64_t addr, uint32_t len, uint32_t sig)
{
    if (!rseq_initialized || !proc)
        return -EINVAL;
    if (addr == 0 || len < sizeof(struct rseq))
        return -EINVAL;
    if (len > PAGE_SIZE) /* sanity check: rseq struct shouldn't span multiple pages */
        return -EINVAL;

    /* Validate the address is in user space */
    if (addr >= USER_VADDR_MAX)
        return -EFAULT;

    struct rseq_state *state = rseq_get_state(proc);
    if (!state)
        return -ENOMEM;

    if (state->registered)
        return -EBUSY;

    state->rseq_addr = addr;
    state->rseq_len = len;
    state->rseq_sig = sig;
    state->registered = 1;

    /* Initialize the user-space rseq structure:
     * cpu_id_start and cpu_id should be set by user space, but we zero them
     * for safety. The kernel will update cpu_id on context switch. */
    /* Note: we can't write to user space directly here without a copy_to_user.
     * In this kernel, we trust user space to have zero-initialized it. */

    return 0;
}

int rseq_unregister(struct process *proc)
{
    if (!rseq_initialized || !proc)
        return -EINVAL;

    struct rseq_state *state = rseq_get_state(proc);
    if (!state || !state->registered)
        return -EINVAL;

    /* Signal the abort by clearing rseq_cs in user space via the rseq_abort path */
    rseq_abort(proc);

    state->rseq_addr = 0;
    state->rseq_len = 0;
    state->rseq_sig = 0;
    state->registered = 0;

    return 0;
}

void rseq_abort(struct process *proc)
{
    if (!rseq_initialized || !proc)
        return;

    struct rseq_state *state = rseq_get_state(proc);
    if (!state || !state->registered)
        return;

    /* Clear the rseq_cs field in the user-space rseq structure.
     * This forces the user code to re-check and not commit the critical section. */
    /* We need to write to user space. In this kernel, we use a simple approach:
     * if the address is in user range, we try to map it temporarily.
     * For now, we use vmm_map_page to handle it. */
    uint64_t addr = state->rseq_addr + __builtin_offsetof(struct rseq, rseq_cs);

    /* Check if address is user-accessible */
    if (addr >= USER_VADDR_MAX)
        return;

    /* Write 0 to rseq_cs atomically */
    *(volatile uint64_t *)addr = 0;
}
