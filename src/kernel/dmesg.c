/* dmesg.c — dmesg_restrict */

#define KERNEL_INTERNAL
#include "dmesg.h"
#include "printf.h"
#include "process.h"

/* dmesg_restrict: when set, only root can read dmesg */
int dmesg_restrict = 1;

void dmesg_init(void) {
    kprintf("[OK] dmesg_restrict initialized (value=%d)\\n", dmesg_restrict);
}

int dmesg_check_access(void) {
    if (!dmesg_restrict)
        return 1;  /* unrestricted: allow */

    struct process *p = process_get_current();
    if (!p) return 1;  /* kernel thread: allow */

    if (p->euid == 0 || p->uid == 0)
        return 1;  /* root: allow */

    return 0;  /* non-root: deny */
}
