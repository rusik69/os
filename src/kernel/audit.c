/* audit.c — Audit subsystem */

#define KERNEL_INTERNAL
#include "audit.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "process.h"

int audit_enabled = 0;

/* Ring buffer */
static char audit_buf[AUDIT_BUF_SIZE];
static int  audit_pos = 0;

void audit_init(void) {
    audit_enabled = 1;
    audit_pos = 0;
    memset(audit_buf, 0, sizeof(audit_buf));
    kprintf("[OK] Audit subsystem initialized\\n");
}

void audit_log_event(const char *msg) {
    if (!audit_enabled || !msg) return;

    uint64_t now = timer_get_ticks();
    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;

    /* Format: "[ticks] pid: msg\n" */
    char tmp[256];
    int n = snprintf(tmp, sizeof(tmp), "[%llu] %u: %s\\n",
                     (unsigned long long)now, pid, msg);
    if (n <= 0) return;

    /* Write to ring buffer */
    for (int i = 0; i < n && audit_pos < AUDIT_BUF_SIZE - 1; i++) {
        audit_buf[audit_pos++] = tmp[i];
    }
    if (audit_pos >= AUDIT_BUF_SIZE) audit_pos = 0;
}

void audit_syscall_entry(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    if (!audit_enabled) return;
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;

    char msg[256];
    int n = snprintf(msg, sizeof(msg), "syscall(%llu) pid=%u\\n",
                     (unsigned long long)num, pid);
    if (n > 0) {
        for (int i = 0; i < n && audit_pos < AUDIT_BUF_SIZE - 1; i++)
            audit_buf[audit_pos++] = msg[i];
        if (audit_pos >= AUDIT_BUF_SIZE) audit_pos = 0;
    }
}

void audit_syscall_exit(uint64_t ret) {
    if (!audit_enabled) return;

    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;

    char msg[256];
    int n = snprintf(msg, sizeof(msg), "syscall_exit=%llu pid=%u\\n",
                     (unsigned long long)ret, pid);
    if (n > 0) {
        for (int i = 0; i < n && audit_pos < AUDIT_BUF_SIZE - 1; i++)
            audit_buf[audit_pos++] = msg[i];
        if (audit_pos >= AUDIT_BUF_SIZE) audit_pos = 0;
    }
}

int audit_read_log(char *buf, int max) {
    if (!buf || max <= 0) return -1;
    int to_copy = audit_pos < max ? audit_pos : max;
    memcpy(buf, audit_buf, to_copy);
    return to_copy;
}
