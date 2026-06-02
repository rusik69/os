#include "waitqueue.h"
#include "process.h"
#include "scheduler.h"
#include "io.h"
#include "string.h"
#include "timer.h"
#include "export.h"

/*
 * Wait queue implementation.
 *
 * Each wait queue holds up to WAITQUEUE_MAX_WAITERS PIDs of BLOCKED
 * processes.  wait_queue_sleep() marks the current process BLOCKED,
 * saves its PID in the queue, and yields the CPU.
 *
 * wait_queue_wake() extracts the oldest PID from the queue and moves
 * it to READY.  wait_queue_wake_all() wakes every waiter.
 *
 * All operations are IRQ-safe: interrupts are disabled while manipulating
 * shared state, then restored to their previous state.
 */

int wait_queue_sleep(struct wait_queue *wq) {
    uint64_t flags;
    struct process *cur = process_get_current();
    if (!cur) return -1;

    spinlock_irqsave_acquire(&wq->lock, &flags);

    if (wq->count >= WAITQUEUE_MAX_WAITERS) {
        spinlock_irqsave_release(&wq->lock, flags);
        return -1;  /* queue full */
    }

    /* Insert at the tail (FIFO) */
    int tail = (wq->head + wq->count) % WAITQUEUE_MAX_WAITERS;
    wq->pids[tail] = cur->pid;
    wq->count++;

    /* Mark process BLOCKED and remove from scheduler */
    cur->state = PROCESS_BLOCKED;
    spinlock_release(&wq->lock);  /* release lock — IRQs still disabled */
    scheduler_remove(cur);

    uint64_t iflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(iflags));
    if (iflags & 0x200) __asm__ volatile("sti");

    /* Yield the CPU — scheduler will pick another process */
    scheduler_yield();

    /* Woken up: re-acquire lock to clean up */
    spinlock_irqsave_acquire(&wq->lock, &iflags);

    /* Find and remove our PID from the queue (in case of wake_all) */
    for (int i = 0; i < wq->count; i++) {
        int idx = (wq->head + i) % WAITQUEUE_MAX_WAITERS;
        if (wq->pids[idx] == cur->pid) {
            /* Compact by moving last element here */
            int last = (wq->head + wq->count - 1) % WAITQUEUE_MAX_WAITERS;
            if (idx != last)
                wq->pids[idx] = wq->pids[last];
            wq->pids[last] = 0;
            wq->count--;
            break;
        }
    }

    spinlock_irqsave_release(&wq->lock, flags);
    return 0;
}

int wait_queue_wake(struct wait_queue *wq) {
    uint64_t flags;
    int woken = 0;

    spinlock_irqsave_acquire(&wq->lock, &flags);

    if (wq->count == 0) {
        spinlock_irqsave_release(&wq->lock, flags);
        return 0;
    }

    /* Dequeue oldest waiter (FIFO) */
    uint32_t pid = wq->pids[wq->head];
    wq->pids[wq->head] = 0;
    wq->head = (wq->head + 1) % WAITQUEUE_MAX_WAITERS;
    wq->count--;

    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].pid == pid && table[i].state == PROCESS_BLOCKED) {
            table[i].state = PROCESS_READY;
            table[i].last_run_tick = 0; /* will be set when scheduled */
            scheduler_wakeup(&table[i]);
            woken = 1;
            break;
        }
    }

    spinlock_irqsave_release(&wq->lock, flags);
    return woken;
}

int wait_queue_wake_all(struct wait_queue *wq) {
    int total = 0;
    while (wait_queue_wake(wq) > 0)
        total++;
    return total;
}

int wait_queue_wake_pid(struct wait_queue *wq, uint32_t pid) {
    uint64_t flags;
    int found = 0;

    spinlock_irqsave_acquire(&wq->lock, &flags);

    for (int i = 0; i < wq->count; i++) {
        int idx = (wq->head + i) % WAITQUEUE_MAX_WAITERS;
        if (wq->pids[idx] == pid) {
            /* Remove by compacting */
            int last = (wq->head + wq->count - 1) % WAITQUEUE_MAX_WAITERS;
            if (idx != last)
                wq->pids[idx] = wq->pids[last];
            wq->pids[last] = 0;
            wq->count--;

            struct process *table = process_get_table();
            for (int j = 0; j < PROCESS_MAX; j++) {
                if (table[j].pid == pid && table[j].state == PROCESS_BLOCKED) {
                    table[j].state = PROCESS_READY;
                    table[j].last_run_tick = 0;
                    scheduler_wakeup(&table[j]);
                    found = 1;
                    break;
                }
            }
            break;
        }
    }

    spinlock_irqsave_release(&wq->lock, flags);
    return found;
}

/* Interruptible wait: like wait_queue_sleep, but returns -EINTR if a
 * signal is pending before sleeping or after being woken. */
int wait_queue_sleep_interruptible(struct wait_queue *wq) {
    uint64_t flags;
    struct process *cur = process_get_current();
    if (!cur) return -1;

    /* Check for pending signals before blocking */
    if (cur->pending_signals) {
        return -4; /* -EINTR */
    }

    spinlock_irqsave_acquire(&wq->lock, &flags);

    if (wq->count >= WAITQUEUE_MAX_WAITERS) {
        spinlock_irqsave_release(&wq->lock, flags);
        return -1;  /* queue full */
    }

    /* Insert at the tail (FIFO) */
    int tail = (wq->head + wq->count) % WAITQUEUE_MAX_WAITERS;
    wq->pids[tail] = cur->pid;
    wq->count++;

    /* Mark process BLOCKED and remove from scheduler */
    cur->state = PROCESS_BLOCKED;
    spinlock_release(&wq->lock);  /* release lock — IRQs still disabled */
    scheduler_remove(cur);

    uint64_t iflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(iflags));
    if (iflags & 0x200) __asm__ volatile("sti");

    /* Yield the CPU — scheduler will pick another process */
    scheduler_yield();

    /* Woken up: re-acquire lock to clean up */
    spinlock_irqsave_acquire(&wq->lock, &iflags);

    /* Find and remove our PID from the queue (in case of wake_all) */
    for (int i = 0; i < wq->count; i++) {
        int idx = (wq->head + i) % WAITQUEUE_MAX_WAITERS;
        if (wq->pids[idx] == cur->pid) {
            /* Compact by moving last element here */
            int last = (wq->head + wq->count - 1) % WAITQUEUE_MAX_WAITERS;
            if (idx != last)
                wq->pids[idx] = wq->pids[last];
            wq->pids[last] = 0;
            wq->count--;
            break;
        }
    }

    spinlock_irqsave_release(&wq->lock, flags);

    /* Check for signals that may have woken us */
    if (cur->pending_signals) {
        return -4; /* -EINTR — caller should handle signal delivery */
    }

    return 0;
}

/* Sleep with timeout: blocks until woken OR 'ticks' timer ticks elapse.
 * Returns 0 if woken, -62 (-ETIME) on timeout, -1 on queue full. */
int wait_queue_sleep_timeout(struct wait_queue *wq, uint64_t ticks) {
    uint64_t flags;
    struct process *cur = process_get_current();
    if (!cur) return -1;

    uint64_t deadline = timer_get_ticks() + ticks;

    spinlock_irqsave_acquire(&wq->lock, &flags);

    if (wq->count >= WAITQUEUE_MAX_WAITERS) {
        spinlock_irqsave_release(&wq->lock, flags);
        return -1;
    }

    int tail = (wq->head + wq->count) % WAITQUEUE_MAX_WAITERS;
    wq->pids[tail] = cur->pid;
    wq->count++;

    /* Set timeout so scheduler_wake_sleepers can wake us */
    cur->sleep_until = deadline;
    cur->state = PROCESS_BLOCKED;
    spinlock_release(&wq->lock);
    scheduler_remove(cur);

    uint64_t iflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(iflags));
    if (iflags & 0x200) __asm__ volatile("sti");

    scheduler_yield();

    /* Woken: re-acquire lock and clean up */
    spinlock_irqsave_acquire(&wq->lock, &iflags);
    int found = 0;
    for (int i = 0; i < wq->count; i++) {
        int idx = (wq->head + i) % WAITQUEUE_MAX_WAITERS;
        if (wq->pids[idx] == cur->pid) {
            int last = (wq->head + wq->count - 1) % WAITQUEUE_MAX_WAITERS;
            if (idx != last)
                wq->pids[idx] = wq->pids[last];
            wq->pids[last] = 0;
            wq->count--;
            found = 1;
            break;
        }
    }
    spinlock_irqsave_release(&wq->lock, flags);

    /* Check if we timed out (sleep_until still set means scheduler_wake_sleepers woke us) */
    if (!found && cur->sleep_until > 0) {
        cur->sleep_until = 0;
        return -62; /* -ETIME */
    }

    return 0;
}

/* Interruptible sleep with timeout */
int wait_queue_sleep_interruptible_timeout(struct wait_queue *wq, uint64_t ticks) {
    uint64_t flags;
    struct process *cur = process_get_current();
    if (!cur) return -1;

    if (cur->pending_signals) return -4; /* -EINTR */

    uint64_t deadline = timer_get_ticks() + ticks;

    spinlock_irqsave_acquire(&wq->lock, &flags);

    if (wq->count >= WAITQUEUE_MAX_WAITERS) {
        spinlock_irqsave_release(&wq->lock, flags);
        return -1;
    }

    int tail = (wq->head + wq->count) % WAITQUEUE_MAX_WAITERS;
    wq->pids[tail] = cur->pid;
    wq->count++;

    cur->sleep_until = deadline;
    cur->state = PROCESS_BLOCKED;
    spinlock_release(&wq->lock);
    scheduler_remove(cur);

    uint64_t iflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(iflags));
    if (iflags & 0x200) __asm__ volatile("sti");

    scheduler_yield();

    spinlock_irqsave_acquire(&wq->lock, &iflags);
    int found = 0;
    for (int i = 0; i < wq->count; i++) {
        int idx = (wq->head + i) % WAITQUEUE_MAX_WAITERS;
        if (wq->pids[idx] == cur->pid) {
            int last = (wq->head + wq->count - 1) % WAITQUEUE_MAX_WAITERS;
            if (idx != last)
                wq->pids[idx] = wq->pids[last];
            wq->pids[last] = 0;
            wq->count--;
            found = 1;
            break;
        }
    }
    spinlock_irqsave_release(&wq->lock, flags);

    if (cur->pending_signals) return -4; /* -EINTR */

    if (!found && cur->sleep_until > 0) {
        cur->sleep_until = 0;
        return -62; /* -ETIME */
    }

    return 0;
}

/* ── Exported symbols for loadable kernel modules ────────────────── */
EXPORT_SYMBOL(wait_queue_sleep);
EXPORT_SYMBOL(wait_queue_wake);
EXPORT_SYMBOL(wait_queue_wake_all);
