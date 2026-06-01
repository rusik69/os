/* kptr_restrict.c — Kernel pointer restrict */

#define KERNEL_INTERNAL
#include "kptr_restrict.h"
#include "printf.h"
#include "process.h"

/* kptr_restrict: 0 = show all kernel addresses, 1 = hide from non-root */
int kptr_restrict = KPTR_RESTRICT_RESTRICTED;

void kptr_restrict_init(void) {
    kprintf("[OK] kptr_restrict initialized (value=%d)\\n", kptr_restrict);
}

int kptr_restrict_check(void) {
    if (kptr_restrict == KPTR_RESTRICT_DISABLED)
        return 0;  /* show all */

    /* Restricted: check if caller is root (uid 0) */
    struct process *p = process_get_current();
    if (!p) return 0;  /* kernel thread: show */

    if (p->euid == 0 || p->uid == 0)
        return 0;  /* root: show */

    return 1;  /* hide */
}
