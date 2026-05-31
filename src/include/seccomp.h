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

/* Initialize seccomp subsystem */
void seccomp_init(void);

/* Check whether a syscall is allowed for the current process's mode.
 * Returns 1 if allowed, 0 if blocked (caller should return -EPERM). */
int seccomp_check_syscall(uint64_t num);

/* Set seccomp mode for the current process. Returns 0 on success. */
int seccomp_set_mode(int mode);

/* Get seccomp mode for the current process. */
int seccomp_get_mode(void);

#endif
