#define KERNEL_INTERNAL
#include "types.h"
#include "seccomp.h"
#include "process.h"
#include "syscall.h"
#include "printf.h"
#include "heap.h"
#include "string.h"

/* Syscalls allowed in STRICT mode (simplified: exactly read/write/exit/sigreturn) */
#define STRICT_ALLOWED_COUNT 4
static const uint64_t strict_allowed[STRICT_ALLOWED_COUNT] = {
    SYS_READ,
    SYS_WRITE,
    SYS_EXIT,
    /* sigreturn would be SYS_RT_SIGRETURN — add here when defined */
    15, /* SYS_RT_SIGRETURN in Linux */
};

void seccomp_init(void) {
    kprintf("[OK] Seccomp initialized\n");
}

int seccomp_check_syscall(uint64_t num) {
    struct process *p = process_get_current();
    if (!p) return 1; /* kernel threads: always allowed */

    int mode = p->seccomp_mode;
    if (mode == SECCOMP_MODE_DISABLED) return 1;

    if (mode == SECCOMP_MODE_STRICT) {
        for (int i = 0; i < STRICT_ALLOWED_COUNT; i++) {
            if (strict_allowed[i] == num) return 1;
        }
        return 0; /* blocked */
    }

    if (mode == SECCOMP_MODE_FILTER) {
        return seccomp_filter_check(num);
    }

    return 1;
}

int seccomp_set_mode(int mode) {
    struct process *p = process_get_current();
    if (!p) return -1;

    /* Once strict or filter is set, it cannot be unset */
    if (p->seccomp_mode != SECCOMP_MODE_DISABLED) return -1;

    if (mode != SECCOMP_MODE_STRICT && mode != SECCOMP_MODE_FILTER) return -1;

    p->seccomp_mode = mode;
    if (mode == SECCOMP_MODE_FILTER) {
        /* Allocate filter if not already present */
        if (!p->seccomp_filter) {
            p->seccomp_filter = (struct seccomp_filter *)kmalloc(sizeof(struct seccomp_filter));
            if (!p->seccomp_filter) return -1;
            memset(p->seccomp_filter, 0, sizeof(struct seccomp_filter));
        }
    }
    return 0;
}

int seccomp_get_mode(void) {
    struct process *p = process_get_current();
    if (!p) return SECCOMP_MODE_DISABLED;
    return p->seccomp_mode;
}

/* ── Filter mode support ────────────────────────────────────────────── */

int seccomp_add_rule(int syscall_nr, uint32_t action) {
    struct process *p = process_get_current();
    if (!p) return -1;
    if (p->seccomp_mode != SECCOMP_MODE_FILTER) return -1;
    if (!p->seccomp_filter) return -1;

    struct seccomp_filter *f = p->seccomp_filter;
    if (f->num_rules >= SECCOMP_FILTER_RULES_MAX) return -1;

    f->rules[f->num_rules].syscall_nr = syscall_nr;
    f->rules[f->num_rules].action = action;
    f->num_rules++;
    return 0;
}

int seccomp_filter_check(uint64_t num) {
    struct process *p = process_get_current();
    if (!p) return 1;
    if (!p->seccomp_filter) return 1; /* no filter rules = allow */

    struct seccomp_filter *f = p->seccomp_filter;
    for (int i = 0; i < f->num_rules; i++) {
        if (f->rules[i].syscall_nr == (int)num) {
            if (f->rules[i].action == SECCOMP_RET_ALLOW)
                return 1;
            else /* SECCOMP_RET_KILL or any other kill action */
                return 0;
        }
    }

    /* Default action for unmatched syscalls: allow (Linux uses SECCOMP_RET_ALLOW) */
    return 1;
}
