#define KERNEL_INTERNAL
#include "pgrp.h"
#include "process.h"
#include "signal.h"
#include "string.h"
#include "printf.h"

/* Process group and session table */
#define PGRP_MAX 64

struct pgrp_entry {
    uint32_t pgid;
    uint32_t member_pids[PROCESS_MAX];
    int member_count;
    int in_use;
};

struct session_entry {
    uint32_t sid;
    uint32_t leader_pid;
    uint32_t member_pids[PROCESS_MAX];
    int member_count;
    int in_use;
};

static struct pgrp_entry pgrp_table[PGRP_MAX];
static struct session_entry session_table[PGRP_MAX];
static int pgrp_initialized = 0;

void pgrp_init(void) {
    if (pgrp_initialized) return;
    memset(pgrp_table, 0, sizeof(pgrp_table));
    memset(session_table, 0, sizeof(session_table));
    pgrp_initialized = 1;
    kprintf("[OK] pgrp initialized\n");
}

int pgrp_create(uint32_t pgid, uint32_t pid) {
    if (!pgrp_initialized) return -1;
    for (int i = 0; i < PGRP_MAX; i++) {
        if (!pgrp_table[i].in_use) {
            pgrp_table[i].pgid = pgid;
            pgrp_table[i].member_pids[0] = pid;
            pgrp_table[i].member_count = 1;
            pgrp_table[i].in_use = 1;
            return 0;
        }
    }
    return -1;
}

int pgrp_join(uint32_t pgid, uint32_t pid) {
    if (!pgrp_initialized) return -1;
    for (int i = 0; i < PGRP_MAX; i++) {
        if (pgrp_table[i].in_use && pgrp_table[i].pgid == pgid) {
            if (pgrp_table[i].member_count < PROCESS_MAX) {
                pgrp_table[i].member_pids[pgrp_table[i].member_count++] = pid;
            }
            /* Update process table */
            struct process *p = process_get_by_pid(pid);
            if (p) p->pgid = pgid;
            return 0;
        }
    }
    /* Create the group if it doesn't exist */
    return pgrp_create(pgid, pid);
}

int pgrp_leave(uint32_t pid) {
    if (!pgrp_initialized) return -1;
    for (int i = 0; i < PGRP_MAX; i++) {
        if (pgrp_table[i].in_use) {
            for (int j = 0; j < pgrp_table[i].member_count; j++) {
                if (pgrp_table[i].member_pids[j] == pid) {
                    /* Remove from list by shifting */
                    for (int k = j; k < pgrp_table[i].member_count - 1; k++)
                        pgrp_table[i].member_pids[k] = pgrp_table[i].member_pids[k + 1];
                    pgrp_table[i].member_count--;
                    return 0;
                }
            }
        }
    }
    return -1;
}

uint32_t pgrp_find(uint32_t pid) {
    if (!pgrp_initialized) return 0;
    for (int i = 0; i < PGRP_MAX; i++) {
        if (pgrp_table[i].in_use) {
            for (int j = 0; j < pgrp_table[i].member_count; j++) {
                if (pgrp_table[i].member_pids[j] == pid)
                    return pgrp_table[i].pgid;
            }
        }
    }
    return 0;
}

int pgrp_is_member(uint32_t pgid, uint32_t pid) {
    if (!pgrp_initialized) return 0;
    for (int i = 0; i < PGRP_MAX; i++) {
        if (pgrp_table[i].in_use && pgrp_table[i].pgid == pgid) {
            for (int j = 0; j < pgrp_table[i].member_count; j++) {
                if (pgrp_table[i].member_pids[j] == pid) return 1;
            }
        }
    }
    return 0;
}

int pgrp_signal(uint32_t pgid, int signum) {
    if (!pgrp_initialized) return -1;
    for (int i = 0; i < PGRP_MAX; i++) {
        if (pgrp_table[i].in_use && pgrp_table[i].pgid == pgid) {
            for (int j = 0; j < pgrp_table[i].member_count; j++) {
                signal_send(pgrp_table[i].member_pids[j], signum);
            }
            return 0;
        }
    }
    return -1;
}

/* Session management */
int session_create(uint32_t sid, uint32_t pid) {
    if (!pgrp_initialized) return -1;
    for (int i = 0; i < PGRP_MAX; i++) {
        if (!session_table[i].in_use) {
            session_table[i].sid = sid;
            session_table[i].leader_pid = pid;
            session_table[i].member_pids[0] = pid;
            session_table[i].member_count = 1;
            session_table[i].in_use = 1;

            struct process *p = process_get_by_pid(pid);
            if (p) p->sid = sid;
            return 0;
        }
    }
    return -1;
}

int session_join(uint32_t sid, uint32_t pid) {
    if (!pgrp_initialized) return -1;
    for (int i = 0; i < PGRP_MAX; i++) {
        if (session_table[i].in_use && session_table[i].sid == sid) {
            if (session_table[i].member_count < PROCESS_MAX) {
                session_table[i].member_pids[session_table[i].member_count++] = pid;
            }
            struct process *p = process_get_by_pid(pid);
            if (p) p->sid = sid;
            return 0;
        }
    }
    return session_create(sid, pid);
}

uint32_t session_find(uint32_t pid) {
    if (!pgrp_initialized) return 0;
    for (int i = 0; i < PGRP_MAX; i++) {
        if (session_table[i].in_use) {
            for (int j = 0; j < session_table[i].member_count; j++) {
                if (session_table[i].member_pids[j] == pid)
                    return session_table[i].sid;
            }
        }
    }
    return 0;
}

/* ── pgrp_enter ─────────────────────────────── */
int pgrp_enter(void *task, int pgrp)
{
    if (!task) return -EINVAL;
    struct process *proc = (struct process *)task;
    return pgrp_join((uint32_t)pgrp, proc->pid);
}
