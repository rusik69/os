#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "pgrp.h"
#include "process.h"
void pgrp_init(void) {
    kprintf("[OK] Process group management initialized\n");
}
int pgrp_create(struct process *leader) {
    if (!leader) return -1;
    leader->pgid = leader->pid;
    leader->sid = leader->pid;
    return 0;
}
int pgrp_join(struct process *proc, uint32_t pgid) {
    if (!proc) return -1;
    proc->pgid = pgid;
    return 0;
}
