/* stubs.c — Host-side stubs for kernel functions needed by printf.c
 *
 * Compiled WITHOUT kernel headers to avoid type conflicts.
 * Types are hard-coded to match the kernel ABI (LP64: unsigned long long = 64-bit).
 */

#include <stddef.h>
#include <stdint.h>

/* ── Kernel heap API ──────────────────────────────────────────────── */
/* Kernel's static inline free()/malloc() in stdlib.h delegate to these. */
void *libc_malloc(unsigned long long size) {
    return __builtin_malloc((unsigned long)size);
}
void libc_free(void *ptr) {
    __builtin_free(ptr);
}
void *kmalloc(unsigned long long size) {
    return __builtin_malloc((unsigned long)size);
}
void kfree(void *ptr) {
    __builtin_free(ptr);
}

/* ── Kernel timer stub ──────────────────────────────────────────── */
unsigned long long timer_get_ticks(void) {
    static unsigned long long fake_tick = 0;
    return fake_tick++;
}

/* ── Serial write stub (printf.c calls this as serial_write(chunk)) ── */
void serial_write(const char *str) {
    (void)str;
}

/* ── kptr_restrict stub (used by printf %pK) ─────────────────── */
__attribute__((weak)) int kptr_restrict_check(void) {
    return 0;  /* allow all pointer prints in test mode */
}

/* ── Lockdown stub (used by cmdline.c) ─────────────────────── */
void lock_down(int level) { (void)level; }

/* ── CMOS NVRAM stubs (used by cmdline.c) ──────────────────── */
unsigned char cmos_nvram_read(unsigned char offset) { (void)offset; return 0; }
void cmos_nvram_write(unsigned char offset, unsigned char val) { (void)offset; (void)val; }

/* ── Audit log stub (used by signal_validate.c) ────────────── */
void audit_log_event(const char *msg) { (void)msg; }

/* ── Process stubs ─────────────────────────────────────────── */
struct process { int pid; int euid; int uid; unsigned char sched_policy; uint8_t priority; uint64_t dl_runtime; uint64_t dl_deadline; uint64_t dl_period; int dl_active; int dl_throttled; };
struct process *process_get_by_pid(uint32_t pid) { (void)pid; return NULL; }
struct process *process_get_current(void) { return NULL; }

/* ── Sysctl stub (used by kptr_restrict.c) ─────────────────── */
void sysctl_register(const char *name,
                     int (*read_fn)(char *, int),
                     int (*write_fn)(const char *, int)) {
    (void)name; (void)read_fn; (void)write_fn;
}

/* ── SCHED_DEADLINE stub (used by sched_attr.c) ─────────────── */
int sched_deadline_add_task(struct process *proc) { (void)proc; return 0; }
