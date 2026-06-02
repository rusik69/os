#ifndef PREEMPT_H
#define PREEMPT_H

/*
 * Preemptible kernel support.
 *
 * Provides fine-grained kernel preemption: long-running kernel code
 * (syscalls, interrupt handlers that return to kernel mode) can be
 * preempted by higher-priority tasks, provided no spinlocks are held.
 *
 * ── Usage ──────────────────────────────────────────────────────────
 *   preempt_disable()       — Increment the per-CPU preemption counter.
 *                             While the counter is > 0, preemption is
 *                             blocked on this CPU.
 *
 *   preempt_enable()        — Decrement the counter.  If it reaches 0
 *                             and a reschedule was requested while
 *                             preemption was disabled, call schedule()
 *                             immediately (this is the main preemption
 *                             point for kernel code).
 *
 *   preempt_enable_no_resched() — Decrement without checking the
 *                             reschedule flag.  Used when the caller
 *                             knows rescheduling is not needed.
 *
 *   preempt_count()         — Return the current preemption depth.
 *
 *   preemptible()           — Return non-zero if preemption is allowed
 *                             (counter == 0).
 *
 *   set_need_resched()      — Request a reschedule on the current CPU.
 *                             The actual context switch happens at the
 *                             next preempt_enable() where the counter
 *                             drops to zero.
 *
 *   clear_need_resched()    — Clear the pending reschedule request.
 *
 *   need_resched()          — Return non-zero if a reschedule is
 *                             pending on the current CPU.
 *
 *   preempt_check_resched() — Convenience: if a reschedule is pending
 *                             and preemption is enabled, call schedule().
 *
 * ── Integration ────────────────────────────────────────────────────
 * - spinlock_acquire() calls preempt_disable() before the spin loop;
 *   spinlock_release()  calls preempt_enable()  after the release.
 *   This ensures a lock holder is never preempted.
 *
 * - scheduler_tick()  calls schedule() directly only if the kernel is
 *   preemptible (preempt_count == 0).  Otherwise it sets the
 *   need_resched flag so the reschedule happens at the next safe
 *   preemption point (preempt_enable()).
 */

#include "types.h"
#include "smp.h"
#include "scheduler.h"

/* ── Preemption control ──────────────────────────────────────────── */

/* Disable preemption on the current CPU.
 * Safe to call nested; each call must be matched by preempt_enable(). */
static inline void preempt_disable(void)
{
    struct cpu_info *ci = get_cpu_info();
    ci->preempt_count++;
    /* Barrier: ensure the preempt_count increment is visible before any
     * subsequent memory operations that might need preemption disabled. */
    __asm__ volatile("" ::: "memory");
}

/* Re-enable preemption.  If the counter drops to zero and a reschedule
 * was requested while preemption was disabled, call schedule() now. */
static inline void preempt_enable(void)
{
    struct cpu_info *ci = get_cpu_info();
    __asm__ volatile("" ::: "memory"); /* barrier before decrement */
    if (--ci->preempt_count == 0 && ci->need_resched) {
        ci->need_resched = 0;
        schedule();
    }
}

/* Re-enable preemption without checking the reschedule flag.
 * Use only when the caller is certain that no reschedule is needed. */
static inline void preempt_enable_no_resched(void)
{
    struct cpu_info *ci = get_cpu_info();
    __asm__ volatile("" ::: "memory");
    ci->preempt_count--;
}

/* Return the current preemption depth (0 = preemption allowed). */
static inline int preempt_count(void)
{
    return get_cpu_info()->preempt_count;
}

/* Return non-zero if preemption is currently allowed on this CPU. */
static inline int preemptible(void)
{
    return get_cpu_info()->preempt_count == 0;
}

/* ── Reschedule request ──────────────────────────────────────────── */

/* Request that schedule() be called at the next safe preemption point. */
static inline void set_need_resched(void)
{
    get_cpu_info()->need_resched = 1;
    __asm__ volatile("mfence" ::: "memory"); /* visible to other CPUs */
}

/* Clear a pending reschedule request. */
static inline void clear_need_resched(void)
{
    get_cpu_info()->need_resched = 0;
    __asm__ volatile("mfence" ::: "memory");
}

/* Return non-zero if a reschedule has been requested on this CPU. */
static inline int need_resched(void)
{
    return get_cpu_info()->need_resched;
}

/* Preemption-point check: if a reschedule is pending and the kernel
 * is preemptible, call schedule() now.  Useful in long-running loops
 * and at strategic points in syscall handlers. */
static inline void preempt_check_resched(void)
{
    struct cpu_info *ci = get_cpu_info();
    if (ci->need_resched && ci->preempt_count == 0) {
        ci->need_resched = 0;
        schedule();
    }
}

#endif /* PREEMPT_H */
