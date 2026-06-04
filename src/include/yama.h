#ifndef YAMA_H
#define YAMA_H

#include "types.h"

/* YAMA ptrace scope values */
#define YAMA_PTRACE_SCOPE_DISABLED   0  /* allow any process to trace */
#define YAMA_PTRACE_SCOPE_RESTRICTED 1  /* only descendants can trace */
#define YAMA_PTRACE_SCOPE_ADMIN      2  /* admin-controlled: CAP_SYS_PTRACE + target opt-in */

/* PR_SET_PTRACER values */
#define PR_SET_PTRACER_PID_ANY      (-1)  /* allow any tracer */
#define PR_SET_PTRACER_PID_NONE      0    /* disable tracer permission (default) */

/* YAMA ptrace scope variable (readable/writable via /proc/sys/kernel/yama/ptrace_scope) */
extern int yama_ptrace_scope;

/* ── API ────────────────────────────────────────────────────────── */

void yama_init(void);
int  yama_ptrace_allowed(uint32_t caller_pid, uint32_t target_pid);
void yama_set_ptracer(uint32_t target_pid, int tracer_pid);
int  yama_get_ptracer(uint32_t target_pid);

#endif /* YAMA_H */
