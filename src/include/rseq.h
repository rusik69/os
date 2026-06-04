#ifndef RSEQ_H
#define RSEQ_H

#include "types.h"
#include "process.h"

/* Restartable sequence per-thread data structure
 * (mirrors linux/rseq.h ABI). */
struct rseq {
    uint32_t cpu_id_start;  /* Initial CPU, updated atomically */
    uint32_t cpu_id;        /* Current CPU number */
    uint64_t rseq_cs;       /* Pointer to struct rseq_cs (or NULL) */
    uint32_t flags;         /* RSEQ_FLAG_* */
    uint32_t padding;
} __attribute__((aligned(32)));

/* rseq_cs header (the abort-critical section descriptor) */
struct rseq_cs {
    uint32_t version;
    uint32_t flags;
    uint64_t start_ip;
    uint64_t post_commit_offset;
    uint64_t abort_ip;
} __attribute__((aligned(32)));

/* Flags */
#define RSEQ_FLAG_UNREGISTER (1 << 0)

/* Register restartable sequences for the current process.
 * addr: userspace address of struct rseq.
 * len: size of struct rseq (must be >= sizeof(struct rseq)).
 * sig: signature for rseq abort handler validation.
 * Returns 0 on success, negative on error. */
int rseq_register(struct process *proc, uint64_t addr, uint32_t len, uint32_t sig);

/* Unregister restartable sequences. */
int rseq_unregister(struct process *proc);

/* Force abort of any current rseq critical section.
 * Clears the rseq_cs pointer so the next attempt will start fresh. */
void rseq_abort(struct process *proc);

/* Update the rseq cpu_id in userspace to reflect the current CPU.
 * Called on every context switch to the target process. */
void rseq_update_cpu_id(struct process *proc);

/* Handle process migration to a different CPU.
 * If the process had an rseq registered and is in a critical section
 * on the old CPU, abort it. */
void rseq_migrate(struct process *proc, int old_cpu, int new_cpu);

/* Initialize rseq subsystem. */
void rseq_init(void);

#endif /* RSEQ_H */
