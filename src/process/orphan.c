#define KERNEL_INTERNAL
#include "process.h"
#include "scheduler.h"
#include "signal.h"
#include "printf.h"
#include "timer.h"

/* Orphan reaper: periodically checks for orphaned processes and
 * reparents them to init (PID 1) or sends SIGHUP/SIGCONT.
 *
 * An orphaned process group is one where the parent of every
 * member is either not in the same session or has already exited.
 * When a session leader exits, SIGHUP + SIGCONT is sent to all
 * processes in the foreground process group.
 */

#define ORPHAN_CHECK_INTERVAL 50  /* ticks between checks */

static int orphan_reaper_running = 0;
static uint64_t last_check_tick = 0;

/* Check if a process group is orphaned */
static int is_orphaned_pgrp(uint32_t pgid) {
    struct process *table = process_get_table();
    int found_parent_in_session = 0;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        if (table[i].pgid != pgid) continue;

        /* Check if this process's parent is in the same session */
        uint32_t ppid = table[i].parent_pid;
        struct process *parent = process_get_by_pid(ppid);
        if (parent && parent->state != PROCESS_UNUSED &&
            parent->sid == table[i].sid) {
            found_parent_in_session = 1;
            break;
        }
    }

    return !found_parent_in_session;
}

/* Reparent orphaned children to init */
static void reparent_orphans(void) {
    struct process *table = process_get_table();
    uint32_t current_pid = process_get_current() ? process_get_current()->pid : 0;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        if (table[i].pid == 1 || table[i].pid == 0) continue;

        /* Check if parent is dead */
        struct process *parent = process_get_by_pid(table[i].parent_pid);
        if (!parent || parent->state == PROCESS_UNUSED || parent->state == PROCESS_ZOMBIE) {
            /* Reparent to init (PID 1) */
            table[i].parent_pid = 1;
        }
    }
}

/* Handle orphaned process group: send SIGHUP then SIGCONT */
static void handle_orphan_pgrp(uint32_t pgid) {
    kprintf("[orphan] orphaning pgrp=%u\n", pgid);
    signal_send_group(pgid, SIGHUP);
    signal_send_group(pgid, SIGCONT);
}

/* Check for session leaders that have exited */
static void check_dead_session_leaders(void) {
    struct process *table = process_get_table();

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        if (table[i].pid == 0 || table[i].pid == 1) continue;
        if (table[i].pid == table[i].sid) {
            /* This is a session leader — if it's dying/zombie, orphan its group */
            if (table[i].state == PROCESS_ZOMBIE) {
                kprintf("[orphan] session leader %u (%s) exiting\n",
                        table[i].pid, table[i].name);
                handle_orphan_pgrp(table[i].pgid);
            }
        }
    }
}

/* Run the orphan reaper checks (called periodically) */
void orphan_reaper_check(void) {
    uint64_t now = timer_get_ticks();
    if (now - last_check_tick < ORPHAN_CHECK_INTERVAL) return;
    last_check_tick = now;

    reparent_orphans();
    check_dead_session_leaders();
}

/* Initialize the orphan reaper */
void orphan_reaper_init(void) {
    last_check_tick = timer_get_ticks();
    orphan_reaper_running = 1;
    kprintf("[OK] orphan reaper initialized\n");
}
