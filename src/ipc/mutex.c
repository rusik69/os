/*
 * mutex.c — Priority Inheritance mutex
 *
 * Implements the Priority Inheritance Protocol (PIP) to prevent
 * priority inversion: when a high-priority task waits on a mutex
 * held by a low-priority task, the holder is temporarily boosted
 * to the waiter's priority. On unlock, the original priority is
 * restored.
 */
#include "mutex.h"
#include "scheduler.h"
#include "process.h"
#include "string.h"

#define MUTEX_MAX 32
#define MUTEX_WAITERS_MAX 8

struct mutex_entry {
    volatile int locked;
    int in_use;
    uint32_t owner_pid;              /* PID of current owner (0 = free) */
    uint8_t  owner_orig_prio;        /* owner's priority before any boost */
    uint8_t  highest_waiter_prio;    /* highest priority among waiters (9 = none) */
    int      waiter_count;
    uint32_t waiter_pids[MUTEX_WAITERS_MAX]; /* PIDs waiting on this mutex */
};

static struct mutex_entry mutexes[MUTEX_MAX];

/* Priority Inheritance boost tracking array */
uint8_t mutex_boost[MUTEX_MAX_PI_BOOST];

static void boost_owner(struct mutex_entry *m, uint8_t waiter_prio);
static void restore_owner(struct mutex_entry *m);

int mutex_init(void) {
    for (int i = 0; i < MUTEX_MAX; i++) {
        __asm__ volatile("cli");
        if (!mutexes[i].in_use) {
            mutexes[i].in_use  = 1;
            mutexes[i].locked  = 0;
            mutexes[i].owner_pid = 0;
            mutexes[i].highest_waiter_prio = 9; /* sentinel > any valid priority */
            mutexes[i].waiter_count = 0;
            __asm__ volatile("sti");
            return i;
        }
        __asm__ volatile("sti");
    }
    return -1;
}

void mutex_lock(int id) {
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use) return;
    struct mutex_entry *m = &mutexes[id];
    struct process *self = process_get_current();
    if (!self) return;

    for (;;) {
        __asm__ volatile("cli");
        if (!m->locked) {
            /* Acquire the mutex */
            m->locked = 1;
            m->owner_pid = self->pid;
            m->owner_orig_prio = self->priority;
            __asm__ volatile("sti");
            return;
        }

        /* Mutex is held by another process — check for priority inheritance */
        struct process *owner = process_get_by_pid(m->owner_pid);
        if (owner && owner->state != PROCESS_UNUSED) {
            /* If waiter has higher priority than current owner's base priority,
             * record the highest waiter priority for potential boost */
            if (self->priority < m->highest_waiter_prio) {
                m->highest_waiter_prio = self->priority;
            }
            /* Boost owner if waiter has higher priority */
            boost_owner(m, self->priority);
        }

        /* Register as waiter */
        if (m->waiter_count < MUTEX_WAITERS_MAX) {
            m->waiter_pids[m->waiter_count++] = self->pid;
        }

        __asm__ volatile("sti");
        scheduler_yield();
    }
}

void mutex_unlock(int id) {
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use) return;
    struct mutex_entry *m = &mutexes[id];

    /* Restore owner's original priority */
    restore_owner(m);

    m->locked = 0;
    m->owner_pid = 0;
    m->waiter_count = 0;
    m->highest_waiter_prio = 9;
}

void mutex_destroy(int id) {
    if (id < 0 || id >= MUTEX_MAX) return;
    struct mutex_entry *m = &mutexes[id];

    /* Restore owner priority before destroying */
    restore_owner(m);

    mutexes[id].in_use  = 0;
    mutexes[id].locked  = 0;
    mutexes[id].owner_pid = 0;
    mutexes[id].waiter_count = 0;
    mutexes[id].highest_waiter_prio = 9;
}

/* ── Priority inheritance helpers ──────────────────────────────────── */

/* Boost the mutex owner to waiter_prio if the waiter has higher priority
 * (lower numeric value = higher priority in this scheduler) */
static void boost_owner(struct mutex_entry *m, uint8_t waiter_prio) {
    struct process *owner = process_get_by_pid(m->owner_pid);
    if (!owner || owner->state == PROCESS_UNUSED) return;

    /* waiter_prio is lower number = higher priority.
     * Boost if waiter_prio < owner->priority (meaning waiter is more important) */
    if (waiter_prio < owner->priority) {
        m->owner_orig_prio = owner->priority; /* save current before boost */
        owner->priority = waiter_prio;
        /* Track the boost */
        if (m->owner_pid < MUTEX_MAX_PI_BOOST)
            mutex_boost[m->owner_pid] = waiter_prio;
    }
}

/* Restore the owner's original (base) priority after unlock */
static void restore_owner(struct mutex_entry *m) {
    struct process *owner = process_get_by_pid(m->owner_pid);
    if (!owner || owner->state == PROCESS_UNUSED) return;

    owner->priority = owner->base_priority;
    m->owner_orig_prio = owner->base_priority;
    /* Clear boost tracking */
    if (m->owner_pid < MUTEX_MAX_PI_BOOST)
        mutex_boost[m->owner_pid] = 0;
}
