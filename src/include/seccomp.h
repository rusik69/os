#ifndef SECCOMP_H
#define SECCOMP_H

#include "types.h"

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
 * When SECCOMP_MODE_STRICT is set, the syscall dispatcher rejects any
 * syscall not in the strict allowlist before executing it.
 */

#define SECCOMP_MODE_DISABLED  0
#define SECCOMP_MODE_STRICT    1
#define SECCOMP_MODE_FILTER    2

/* Seccomp filter actions */
#define SECCOMP_RET_KILL  0x80000000U
#define SECCOMP_RET_ALLOW 0x7fff0000U

#define SECCOMP_FILTER_RULES_MAX 32

/* Seccomp filter rule */
struct seccomp_rule {
    int     syscall_nr;
    uint32_t action; /* SECCOMP_RET_KILL or SECCOMP_RET_ALLOW */
};

/* Per-process seccomp filter */
struct seccomp_filter {
    int num_rules;
    struct seccomp_rule rules[SECCOMP_FILTER_RULES_MAX];
};

/* Initialize seccomp subsystem */
void seccomp_init(void);

/* Check whether a syscall is allowed for the current process's mode.
 * Returns 1 if allowed, 0 if blocked (caller should return -EPERM). */
int seccomp_check_syscall(uint64_t num);

/* Set seccomp mode for the current process. Returns 0 on success. */
int seccomp_set_mode(int mode);

/* Get seccomp mode for the current process. */
int seccomp_get_mode(void);

/* Add a seccomp filter rule for the current process */
int seccomp_add_rule(int syscall_nr, uint32_t action);

/* Check syscall against the seccomp filter (mode FILTER) */
int seccomp_filter_check(uint64_t num);

#endif
