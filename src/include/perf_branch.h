#ifndef PERF_BRANCH_H
#define PERF_BRANCH_H

#include "types.h"

/*
 * perf_branch.h — perf branch stack (LBR) public API
 *
 * Supports Last Branch Record (LBR) sampling for performance
 * monitoring. The LBR MSRs are per-CPU and must be saved/restored
 * across context switches to prevent task A's branch history from
 * corrupting task B's (and vice versa).
 *
 * Context switch protocol (called with IRQs DISABLED):
 *   1. perf_branch_save_state()   — saves DEBUGCTLMSR + LBR entries to per-CPU area
 *   2. context_switch(old, new)   — switches tasks
 *   3. perf_branch_restore_state()— restores DEBUGCTLMSR + LBR entries from per-CPU area
 *
 * Single-entry read (interrupt-safe):
 *   perf_branch_read_lbr_atomic(out, max) — disables IRQs internally, reads
 *     the LBR MSR ring, and returns consistent entries for the current CPU.
 */

#define PERF_LBR_MAX_ENTRIES 32

struct perf_lbr_entry {
    uint64_t from;
    uint64_t to;
    uint64_t flags; /* branch type, prediction, etc. */
};

/* Per-CPU LBR save area for context-switch save/restore */
struct perf_lbr_save_area {
    uint64_t debugctl;                          /* saved MSR_IA32_DEBUGCTLMSR value */
    struct perf_lbr_entry entries[PERF_LBR_MAX_ENTRIES]; /* saved LBR entries */
    int      count;                             /* number of valid entries saved */
    int      enabled;                           /* was LBR enabled before switch? */
};

/* ── Context-switch hooks (call with IRQs DISABLED) ───────────── */

/* Save LBR MSRs to the current CPU's save area.
 * Must be called before context_switch() with IRQs disabled. */
void perf_branch_save_state(void);

/* Restore LBR MSRs from the current CPU's save area.
 * Must be called after context_switch() returns, before IRQs are restored. */
void perf_branch_restore_state(void);

/* ── LBR sampling API ─────────────────────────────────────────── */

/* Enable LBR sampling on the current CPU.
 * Returns 0 on success, negative errno on failure. */
int perf_branch_enable(void);

/* Disable LBR sampling on the current CPU.
 * Returns 0 on success, negative errno on failure. */
int perf_branch_disable(void);

/* Atomically read LBR entries from the current CPU.
 * Interrupts are disabled internally to ensure a consistent snapshot
 * of the MSR ring.  Returns the number of entries read, or negative errno. */
int perf_branch_read_lbr_atomic(struct perf_lbr_entry *entries, int max);

/* Initialize LBR subsystem (called once during kernel boot). */
void perf_branch_init(void);

#endif /* PERF_BRANCH_H */
