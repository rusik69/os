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

/* ── Terminal foreground process group ──────────────────────────── */
static uint64_t fg_pgid = 0;

void pgrp_set_foreground(uint64_t pgid)
{
    fg_pgid = pgid;
}

uint64_t pgrp_get_foreground(void)
{
    return fg_pgid;
}

/* ── Stub: pgrp_enter ─────────────────────────────── */
static int pgrp_enter(void *task, int pgrp)
{
    (void)task;
    (void)pgrp;
    kprintf("[pgrp] pgrp_enter: not yet implemented\n");
    return 0;
}
/* ── Stub: pgrp_leave ─────────────────────────────── */
static int pgrp_leave(void *task)
{
    (void)task;
    kprintf("[pgrp] pgrp_leave: not yet implemented\n");
    return 0;
}
/* ── Stub: pgrp_signal ─────────────────────────────── */
static int pgrp_signal(int pgrp, int sig)
{
    (void)pgrp;
    (void)sig;
    kprintf("[pgrp] pgrp_signal: not yet implemented\n");
    return 0;
}
