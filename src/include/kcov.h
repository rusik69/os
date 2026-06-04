#ifndef KCOV_H
#define KCOV_H

/*
 * kcov.h — Kernel code coverage for fuzzing (Item 208)
 *
 * Provides coverage collection kernel-side, modelled after Linux KCOV.
 * When a process enables KCOV, the kernel records PC (program counter)
 * values of executed basic blocks into a per-task buffer.  The buffer
 * is then readable by the process to determine which code paths were
 * exercised during a fuzzing run.
 *
 * Interface (simplified vs. Linux KCOV):
 *   syscall(SYS_KCOV, KCOV_INIT_TRACE, size);
 *     - Allocate a coverage buffer of `size` entries on the calling process.
 *   syscall(SYS_KCOV, KCOV_ENABLE, mode);
 *     - Start collecting coverage.  mode must be KCOV_TRACE_PC.
 *   syscall(SYS_KCOV, KCOV_DISABLE, 0);
 *     - Stop collecting coverage.
 *
 * The buffer is exposed at a known virtual address in the process's
 * address space so userspace can read it directly (via the address
 * stored in the kcov_area field).  For this kernel, coverage data is
 * written to the per-process kcov buffer at each syscall entry,
 * giving fuzzers coverage-guided feedback.
 *
 * Buffer layout (Linux-compatible):
 *   [0] = total number of valid entries (n)
 *   [1..n] = PC values of executed basic blocks
 */

#include "types.h"

/* ── ioctl-like commands (passed as arg1 to SYS_KCOV) ──────────── */
#define KCOV_INIT_TRACE   1   /* initialise coverage buffer with N entries */
#define KCOV_ENABLE       2   /* start collecting coverage */
#define KCOV_DISABLE      3   /* stop collecting coverage */

/* ── Coverage trace modes (passed with KCOV_ENABLE) ────────────── */
#define KCOV_TRACE_PC     0   /* trace program counter values */

/* ── Per-process KCOV state ────────────────────────────────────── */
enum kcov_mode {
    KCOV_MODE_NONE     = 0,   /* KCOV not in use */
    KCOV_MODE_INIT     = 1,   /* buffer allocated, not enabled */
    KCOV_MODE_TRACE_PC = 2,   /* coverage collection active */
};

/* Maximum number of coverage entries per process */
#define KCOV_MAX_ENTRIES  (1024 * 128)  /* 128K entries */

/* ── Kernel-internal API ───────────────────────────────────────── */
#ifdef KERNEL_INTERNAL

/* Forward declaration (included from process.h) */
struct process;

/* Initialise KCOV for the current process's kcov fields. */
void kcov_process_init(struct process *proc);

/* Record a coverage entry (called from instrumentation points). */
void kcov_record(uint64_t pc);

/* Syscall handler (called from syscall dispatch). */
int sys_kcov(uint64_t cmd, uint64_t arg2);

/* Free KCOV resources on process exit. */
void kcov_process_exit(struct process *proc);

#endif /* KERNEL_INTERNAL */

#endif /* KCOV_H */
