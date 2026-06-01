#ifndef YAMA_H
#define YAMA_H

#include "types.h"

/* YAMA ptrace scope values */
#define YAMA_PTRACE_SCOPE_DISABLED   0
#define YAMA_PTRACE_SCOPE_RESTRICTED 1

/* YAMA ptrace scope variable (readable/writable via /proc/sys/kernel/yama/ptrace_scope) */
extern int yama_ptrace_scope;

/* ── API ────────────────────────────────────────────────────────── */

void yama_init(void);
int  yama_ptrace_allowed(uint32_t caller_pid, uint32_t target_pid);

#endif /* YAMA_H */
