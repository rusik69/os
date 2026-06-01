#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "process.h"
#include "coredump.h"
#include "string.h"
#include "fs.h"
#include "vfs.h"
static int coredump_enabled = 1;
void coredump_init(void) {
    kprintf("[OK] Core dump handler initialized\n");
}
void coredump_set_enabled(int en) { coredump_enabled = en; }
int coredump_generate(struct process *proc) {
    if (!coredump_enabled || !proc) return -1;
    kprintf("[coredump] Generating core for PID %d (%s)\n", proc->pid, proc->name);
    /* TODO: implement core file generation */
    kprintf("[coredump] Core dump stub\n");
    return 0;
}
