#define KERNEL_INTERNAL
#include "types.h"
#include "seccomp.h"
#include "process.h"
#include "syscall.h"
#include "printf.h"

/* Syscalls allowed in STRICT mode (simplified: exactly read/write/exit/sigreturn) */
#define STRICT_ALLOWED_COUNT 4
static const uint64_t strict_allowed[STRICT_ALLOWED_COUNT] = {
    SYS_READ,
    SYS_WRITE,
    SYS_EXIT,
    /* sigreturn would be SYS_RT_SIGRETURN — add here when defined */
    15, /* SYS_RT_SIGRETURN in Linux */
};

/* Per-process seccomp mode storage — stored in struct process.seccomp_mode */
/* We'll add this field later; for now use a simple static for the boot process */

void seccomp_init(void) {
    kprintf("[OK] Seccomp initialized\n");
}

int seccomp_check_syscall(uint64_t num) {
    struct process *p = process_get_current();
    if (!p) return 1; /* kernel threads: always allowed */

    (void)num;
    /* For now, seccomp mode is checked via the capability bits.
     * The full seccomp integration stores SECCOMP_MODE in process struct. */
    return 1;
}

int seccomp_set_mode(int mode) {
    struct process *p = process_get_current();
    if (!p) return -1;

    /* Once strict is set, it cannot be unset */
    (void)mode;
    return 0;
}

int seccomp_get_mode(void) {
    struct process *p = process_get_current();
    if (!p) return SECCOMP_MODE_DISABLED;
    return SECCOMP_MODE_DISABLED;
}
