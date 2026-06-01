/* yama.c — YAMA ptrace security */

#define KERNEL_INTERNAL
#include "yama.h"
#include "printf.h"
#include "process.h"

/* YAMA ptrace scope: 0 = disabled (allow all), 1 = restricted (descendants only) */
int yama_ptrace_scope = YAMA_PTRACE_SCOPE_RESTRICTED;

void yama_init(void) {
    kprintf("[OK] YAMA ptrace initialized (scope=%d)\\n", yama_ptrace_scope);
}

int yama_ptrace_allowed(uint32_t caller_pid, uint32_t target_pid) {
    if (yama_ptrace_scope == YAMA_PTRACE_SCOPE_DISABLED)
        return 1;

    /* Restricted mode: only allow tracing of descendants */
    struct process *caller = process_get_by_pid(caller_pid);
    struct process *target = process_get_by_pid(target_pid);
    if (!caller || !target) return 0;

    /* Check if target is a descendant of caller */
    struct process *p = target;
    while (p && p->parent_pid != p->pid) {  /* follow parents up */
        if (p->parent_pid == caller_pid)
            return 1;
        p = process_get_by_pid(p->parent_pid);
    }
    return 0;
}
