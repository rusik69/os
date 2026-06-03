#ifndef MUTEX_H
#define MUTEX_H

#include "types.h"

/* Kernel mutex — spin+yield, safe on single-CPU preemptive kernel. */

int  mutex_init(void);
void mutex_lock(int id);
void mutex_unlock(int id);
void mutex_destroy(int id);

/* ── Owner tracking / debug helpers ───────────────────────────── */

/* Returns PID of current mutex owner, or 0 if unlocked/invalid. */
uint32_t mutex_owner(int id);

/* Returns name of current mutex owner, or NULL if unlocked/invalid. */
const char *mutex_owner_name(int id);

/* Returns RIP where current owner acquired the lock, or 0. */
uint64_t mutex_owner_rip(int id);

/* Returns 1 if mutex is locked, 0 if free (or invalid id). */
int mutex_is_locked(int id);

/* Priority Inheritance tracking — boost array indexed by PID */
#define MUTEX_MAX_PI_BOOST 256
extern uint8_t mutex_boost[MUTEX_MAX_PI_BOOST];

/* ── Optimistic spinning statistics ────────────────────────────── */
void mutex_spin_stats(uint64_t *attempts, uint64_t *success,
                       uint64_t *abandoned, uint64_t *timeout);

#endif
