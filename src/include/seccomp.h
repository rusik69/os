#ifndef SECCOMP_H
#define SECCOMP_H

#include "types.h"
#include "signal.h"

/*
 * Seccomp: syscall sandboxing for processes.
 *
 * Modes:
 *   SECCOMP_MODE_DISABLED  — no filtering (default)
 *   SECCOMP_MODE_STRICT    — only allowed syscalls: read, write, exit, sigreturn
 *   SECCOMP_MODE_FILTER    — BPF-like filter (simplified: allowlist bitmap)
 *
 * Per-process seccomp state is tracked in struct process.
 *
 * Syscalls (via prctl):
 *   PR_SET_SECCOMP = 22  — (op, mode)
 *   PR_GET_SECCOMP = 23  — returns current mode
 *
 * Filter actions (Linux-compatible values):
 *   RET_KILL  — kill the calling process immediately
 *   RET_TRAP  — send SIGSYS with seccomp data
 *   RET_LOG   — log the syscall via audit and allow it
 *   RET_ALLOW — allow the syscall
 */

#define SECCOMP_MODE_DISABLED  0
#define SECCOMP_MODE_STRICT    1
#define SECCOMP_MODE_FILTER    2

/* Flags for seccomp_set_mode */
#define SECCOMP_FILTER_FLAG_TSYNC  1U  /* synchronize filters to all threads */

/* Filter actions (Linux-compatible high bits) */
#define SECCOMP_RET_KILL  0x80000000U
#define SECCOMP_RET_TRAP  0x00030000U
#define SECCOMP_RET_LOG   0x7ffc0000U
#define SECCOMP_RET_ALLOW 0x7fff0000U

/* seccomp_data is defined in seccomp_bpf.h */
#include "seccomp_bpf.h"

#define SECCOMP_FILTER_RULES_MAX 32

/* Seccomp filter rule */
struct seccomp_rule {
    int     syscall_nr;
    uint32_t action; /* SECCOMP_RET_KILL, _TRAP, _LOG, or _ALLOW */
};

/* Per-process seccomp filter */
struct seccomp_filter {
    int num_rules;
    struct seccomp_rule rules[SECCOMP_FILTER_RULES_MAX];
};

/* Initialize seccomp subsystem */
void seccomp_init(void);

/*
 * Evaluate a syscall against the current process's seccomp policy.
 * Performs audit logging for LOG/KILL/TRAP actions internally.
 *
 * Returns one of:
 *   SECCOMP_RET_ALLOW — syscall is allowed (pass through)
 *   SECCOMP_RET_KILL  — process must be killed
 *   SECCOMP_RET_TRAP  — send SIGSYS with seccomp_data to process
 *   SECCOMP_RET_LOG   — already logged, syscall is allowed
 *
 * @num     syscall number
 * @a1-a3   first three syscall arguments (logged for audit)
 * @rip     instruction pointer at time of syscall (for TRAP siginfo)
 */
uint32_t seccomp_evaluate_syscall(uint64_t num, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t rip);

/*
 * Legacy check — returns 1 if allowed, 0 if blocked.
 * Calls seccomp_evaluate_syscall internally and handles KILL/TRAP.
 * NOTE: TRAP sends SIGSYS and returns 0; KILL does not return.
 */
int seccomp_check_syscall(uint64_t num);

/* Set seccomp mode for the current process.
 * @mode   seccomp mode (SECCOMP_MODE_STRICT or SECCOMP_MODE_FILTER)
 * @flags  bitmask of SECCOMP_FILTER_FLAG_* (e.g., SECCOMP_FILTER_FLAG_TSYNC)
 * Returns 0 on success. */
int seccomp_set_mode(int mode, unsigned int flags);

/* Get seccomp mode for the current process. */
int seccomp_get_mode(void);

/* Add a seccomp filter rule for the current process */
int seccomp_add_rule(int syscall_nr, uint32_t action);

/* Check syscall against the seccomp filter (mode FILTER) */
int seccomp_filter_check(uint64_t num);

/* Synchronize the current process's seccomp filter to all threads
 * in the same thread group (tgid).  Each thread gets its own copy
 * of the filter rules.  Returns 0 on success. */
int seccomp_tsync(void);

/* Send SIGSYS with seccomp data to the current process (RET_TRAP action).
 * This fills in a siginfo with si_code=SI_KERNEL and seccomp data. */
void seccomp_send_sigsys(uint64_t num, uint64_t rip);

#endif
