/*
 * futex.c — Fast Userspace Mutex (futex) implementation
 *
 * Provides futex_wait and futex_wake operations with hash-based
 * bucketing for efficient lookup. Supports FUTEX_WAIT, FUTEX_WAKE,
 * FUTEX_REQUEUE, and FUTEX_CMP_REQUEUE.
 *
 * Each futex is identified by a (uaddr, private) tuple, hashed
 * into a fixed-size hash table with per-bucket wait queues.
 * Processes block using wait_queue_sleep_interruptible_timeout()
 * and are woken via wait_queue_wake().
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "scheduler.h"
#include "waitqueue.h"
#include "spinlock.h"
#include "heap.h"
#include "errno.h"
#include "timer.h"

/* ── Constants ─────────────────────────────────────────────────────────── */

#define FUTEX_HASH_BITS       8
#define FUTEX_HASH_SIZE       (1U << FUTEX_HASH_BITS)   /* 256 buckets */
#define FUTEX_BUCKET_MASK     (FUTEX_HASH_SIZE - 1)

/* ── Hash bucket ───────────────────────────────────────────────────────── */

struct futex_bucket {
    spinlock_t      lock;
    struct wait_queue wq;
    int             waiters;         /* approximate count for diagnostics */
};

/* ── Global state ──────────────────────────────────────────────────────── */

static struct futex_bucket futex_buckets[FUTEX_HASH_SIZE];
static int futex_initialised;

/* ── Hash function ──────────────────────────────────────────────────────── */

/* Jenkins one-at-a-time hash of (uaddr, private_flag) */
static uint32_t futex_hash(uint32_t uaddr, int private)
{
    uint32_t h = 0;
    uint8_t buf[5];
    buf[0] = (uint8_t)(uaddr & 0xFF);
    buf[1] = (uint8_t)((uaddr >> 8) & 0xFF);
    buf[2] = (uint8_t)((uaddr >> 16) & 0xFF);
    buf[3] = (uint8_t)((uaddr >> 24) & 0xFF);
    buf[4] = (uint8_t)(private & 0xFF);

    for (int i = 0; i < 5; i++) {
        h += buf[i];
        h += (h << 10);
        h ^= (h >> 6);
    }
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);
    return h & FUTEX_BUCKET_MASK;
}

/* ── Initialisation ────────────────────────────────────────────────────── */

void futex_init(void)
{
    if (futex_initialised)
        return;

    for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
        spinlock_init(&futex_buckets[i].lock);
        wait_queue_init(&futex_buckets[i].wq);
        futex_buckets[i].waiters = 0;
    }

    futex_initialised = 1;
    kprintf("[futex] Initialised with %d hash buckets\n", FUTEX_HASH_SIZE);
}

/* ── FUTEX_WAIT implementation ──────────────────────────────────────────── */

/*
 * futex_wait: Atomically check that *uaddr == val, then sleep until
 * woken by futex_wake.  Returns 0 on wake-up, -EAGAIN if *uaddr != val,
 * -EINTR if interrupted by a signal.
 */
int futex_wait(uint32_t *uaddr, uint32_t val, uint64_t timeout_ns, int private)
{
    if (!futex_initialised || !uaddr)
        return -EINVAL;

    uint32_t bucket_idx = futex_hash((uint32_t)(uintptr_t)uaddr, private);
    struct futex_bucket *bucket = &futex_buckets[bucket_idx];

    struct process *cur = process_get_current();
    if (!cur)
        return -EINVAL;

    /* Read the user-space futex word */
    uint32_t current_val;
    memcpy(&current_val, uaddr, sizeof(current_val));

    if (current_val != val)
        return -EAGAIN;

    /* Mark process as blocked */
    cur->state = PROCESS_BLOCKED;
    scheduler_remove(cur);

    /* Use the bucket's wait queue to sleep.
     * Each bucket serves as an indirect wakeup channel. */
    int ret;
    if (timeout_ns > 0) {
        uint64_t timeout_ticks = timeout_ns / 10000000ULL; /* 10ms per tick */
        ret = wait_queue_sleep_interruptible_timeout(&bucket->wq, timeout_ticks);
    } else {
        ret = wait_queue_sleep_interruptible(&bucket->wq);
    }

    if (ret < 0)
        return -EINTR;

    return 0;
}

/* ── FUTEX_WAKE implementation ──────────────────────────────────────────── */

/*
 * futex_wake: Wake up to `nr_wake` waiters on the futex at uaddr.
 * Returns the number of waiters woken.
 */
int futex_wake(uint32_t *uaddr, int nr_wake, int private)
{
    if (!futex_initialised || !uaddr || nr_wake <= 0)
        return 0;

    uint32_t bucket_idx = futex_hash((uint32_t)(uintptr_t)uaddr, private);
    struct futex_bucket *bucket = &futex_buckets[bucket_idx];

    spinlock_acquire(&bucket->lock);

    int woken = 0;
    for (int i = 0; i < nr_wake; i++) {
        int ret = wait_queue_wake(&bucket->wq);
        if (ret <= 0)
            break;
        woken++;
    }

    spinlock_release(&bucket->lock);
    return woken;
}

/* ── FUTEX_REQUEUE implementation ───────────────────────────────────────── */

/*
 * futex_requeue: Wake up to nr_wake waiters, then requeue up to
 * nr_requeue additional waiters from the source futex to the target
 * futex.  In this simplified implementation, requeued tasks are
 * simply woken (full requeueing would require internal wait queue
 * management).
 */
int futex_requeue(uint32_t *uaddr, int nr_wake, int nr_requeue,
                   uint32_t *uaddr2, int private)
{
    if (!futex_initialised || !uaddr || !uaddr2)
        return -EINVAL;

    uint32_t src_bucket = futex_hash((uint32_t)(uintptr_t)uaddr, private);
    uint32_t dst_bucket = futex_hash((uint32_t)(uintptr_t)uaddr2, private);

    struct futex_bucket *src = &futex_buckets[src_bucket];

    /* Lock src bucket */
    spinlock_acquire(&src->lock);

    int total = 0;
    /* Wake up to nr_wake waiters */
    for (int i = 0; i < nr_wake; i++) {
        int ret = wait_queue_wake(&src->wq);
        if (ret <= 0) break;
        total++;
    }

    /* For requeue: in a full implementation, we'd move waiters from
     * src->wq to the target bucket's wq.  For now, we also wake
     * the remaining requeued tasks so they re-evaluate. */
    for (int i = 0; i < nr_requeue; i++) {
        int ret = wait_queue_wake(&src->wq);
        if (ret <= 0) break;
        total++;
    }

    spinlock_release(&src->lock);
    return total;
}

/* ── FUTEX_CMP_REQUEUE implementation ───────────────────────────────────── */

/*
 * futex_cmp_requeue: Like futex_requeue, but only requeues if *uaddr == val.
 */
int futex_cmp_requeue(uint32_t *uaddr, uint32_t val, int nr_wake,
                       int nr_requeue, uint32_t *uaddr2, int private)
{
    if (!futex_initialised || !uaddr || !uaddr2)
        return -EINVAL;

    uint32_t current_val;
    memcpy(&current_val, uaddr, sizeof(current_val));
    if (current_val != val)
        return -EAGAIN;

    return futex_requeue(uaddr, nr_wake, nr_requeue, uaddr2, private);
}

/* ── Diagnostics ───────────────────────────────────────────────────────── */

void futex_dump(void)
{
    if (!futex_initialised) {
        kprintf("[futex] Not initialised\n");
        return;
    }

    int total_waiters = 0;
    for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
        if (futex_buckets[i].waiters > 0) {
            kprintf("[futex] bucket[%d]: %d waiters\n",
                    i, futex_buckets[i].waiters);
            total_waiters += futex_buckets[i].waiters;
        }
    }
    kprintf("[futex] Total waiters: %d\n", total_waiters);
}

/* ── futex_lock_pi ─────────────────────────────────────── */
int futex_lock_pi(uint32_t *uaddr, int private)
{
    if (!futex_initialised || !uaddr)
        return -EINVAL;

    struct process *cur = process_get_current();
    if (!cur)
        return -EINVAL;

    uint32_t tid = cur->pid;
    uint32_t zero = 0;

    for (;;) {
        uint32_t cur_val;
        memcpy(&cur_val, uaddr, sizeof(cur_val));

        if (cur_val == 0) {
            /* Lock is free — try to acquire by writing our PID */
            __asm__ volatile("cli");
            memcpy(&cur_val, uaddr, sizeof(cur_val));
            if (cur_val == 0) {
                /* Still free — take it */
                uint32_t new_val = tid;
                memcpy(uaddr, &new_val, sizeof(new_val));
                __asm__ volatile("sti");
                return 0;
            }
            __asm__ volatile("sti");
            /* Someone else took it — re-evaluate */
            continue;
        }

        /* Lock is held — set the FUTEX_WAITERS bit (bit 31) to indicate contention */
        uint32_t waiters_val = cur_val | 0x80000000U;
        memcpy(uaddr, &waiters_val, sizeof(waiters_val));

        /* Wait for the lock to be released */
        int ret = futex_wait(uaddr, waiters_val, 0, private);
        if (ret < 0 && ret != -EAGAIN) {
            /* Signal interrupted */
            return ret;
        }
        /* Retry the loop */
    }
}

/* ── futex_unlock_pi ───────────────────────────────────── */
int futex_unlock_pi(uint32_t *uaddr, int private)
{
    if (!futex_initialised || !uaddr)
        return -EINVAL;

    struct process *cur = process_get_current();
    if (!cur)
        return -EINVAL;

    uint32_t cur_val;
    memcpy(&cur_val, uaddr, sizeof(cur_val));

    /* Check we are the owner (bits 0-30 contain PID) */
    if ((cur_val & 0x7FFFFFFF) != cur->pid)
        return -EPERM;

    /* Clear the lock word */
    uint32_t zero = 0;
    memcpy(uaddr, &zero, sizeof(zero));

    /* Wake up one waiter */
    futex_wake(uaddr, 1, private);

    return 0;
}
