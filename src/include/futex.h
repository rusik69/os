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
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_WAIT_PRIVATE   (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE   (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_BITSET_PRIVATE   (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_BITSET_PRIVATE   (FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG)

/* ── FUTEX_WAKE_OP: atomic operations on uaddr2 ─────────────────── */
#define FUTEX_OP_SET         0  /* uaddr2 = oparg */
#define FUTEX_OP_ADD         1  /* uaddr2 += oparg */
#define FUTEX_OP_OR          2  /* uaddr2 |= oparg */
#define FUTEX_OP_ANDN        3  /* uaddr2 &= ~oparg */
#define FUTEX_OP_XOR         4  /* uaddr2 ^= oparg */
#define FUTEX_OP_OPARG_SHIFT 8  /* if set, oparg << 8 before use */

/* ── FUTEX_WAKE_OP: comparison operators ─────────────────────────── */
#define FUTEX_OP_CMP_EQ      0  /* oldval == cmparg */
#define FUTEX_OP_CMP_NE      1  /* oldval != cmparg */
#define FUTEX_OP_CMP_LT      2  /* oldval < cmparg */
#define FUTEX_OP_CMP_LE      3  /* oldval <= cmparg */
#define FUTEX_OP_CMP_GT      4  /* oldval > cmparg */
#define FUTEX_OP_CMP_GE      5  /* oldval >= cmparg */

/* ── FUTEX_WAKE_OP: encode val3 from op/cmp/oparg/cmparg ────────── */
#define FUTEX_OP_ENCODE(op, oparg, cmp, cmparg) \
    ((((op) & 0xf) << 28) | (((cmp) & 0xf) << 24) | \
     (((oparg) & 0xfff) << 12) | ((cmparg) & 0xfff))

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

/* ── Robust list cleanup (called from process_cleanup) ──────────── */
void futex_robust_list_cleanup(struct process *proc);

/* ── Global futex state ──────────────────────────────────────────── */
#define FUTEX_MAX_WAITERS 64

struct futex_waiter {
    uint32_t *uaddr;
    struct process *proc;
    uint32_t bitset;         /* for FUTEX_WAIT_BITSET/FUTEX_WAKE_BITSET */
};

extern struct futex_waiter futex_waiters[FUTEX_MAX_WAITERS];
extern int futex_num_waiters;

#endif /* FUTEX_H */
