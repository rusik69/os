#ifndef AUDIT_H
#define AUDIT_H

#include "types.h"

/* Audit ring buffer size (8KB) */
#define AUDIT_BUF_SIZE 8192

/* Audit enable flag */
extern int audit_enabled;

/* ── API ────────────────────────────────────────────────────────── */

void audit_init(void);
void audit_log_event(const char *msg);
void audit_syscall_entry(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5);
void audit_syscall_exit(uint64_t ret);
int  audit_read_log(char *buf, int max);

#endif /* AUDIT_H */
