#ifndef FUTEX_H
#define FUTEX_H

#include "types.h"
#include "process.h"

/* ── Futex operations ────────────────────────────────────────────── */
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_LOCK_PI         6
#define FUTEX_UNLOCK_PI       7
#define FUTEX_TRYLOCK_PI      8
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_CMP_REQUEUE_PI 12
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_WAIT_PRIVATE   (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE   (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

/* ── Robust list (for robust futex) ──────────────────────────────── */
struct robust_list {
    struct robust_list *next;
};

struct robust_list_head {
    struct robust_list list;
    long               futex_offset;
    struct robust_list *list_op_pending;
};

/* ── PI futex state ──────────────────────────────────────────────── */
#define FUTEX_PI_MAX_WAITERS 8

struct futex_pi_state {
    uint32_t     *uaddr;         /* userspace address */
    uint32_t      owner_pid;     /* current owner PID */
    int           in_use;
    int           waiter_count;
    uint32_t      waiter_pids[FUTEX_PI_MAX_WAITERS];
};

/* ── Syscall prototypes ──────────────────────────────────────────── */
int sys_set_robust_list(struct robust_list_head *head, size_t len);
int sys_get_robust_list(int pid, struct robust_list_head **head_ptr, size_t *len_ptr);

/* ── PI futex helpers ────────────────────────────────────────────── */
void futex_pi_free(uint32_t *uaddr);
int  futex_pi_lock(uint32_t *uaddr, uint32_t owner_pid);
int  futex_pi_unlock(uint32_t *uaddr, uint32_t owner_pid);
void futex_pi_boost_owner(uint32_t *uaddr);

/* ── Global futex state ──────────────────────────────────────────── */
#define FUTEX_MAX_WAITERS 64

struct futex_waiter {
    uint32_t *uaddr;
    struct process *proc;
};

extern struct futex_waiter futex_waiters[FUTEX_MAX_WAITERS];
extern int futex_num_waiters;

#endif /* FUTEX_H */
