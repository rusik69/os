/* Spinlock stubs for host-side testing.
 * Shadows the kernel's spinlock.h include guard so that the real
 * kernel spinlock.h (which contains privileged instructions) is
 * skipped despite being found first (same directory as klist.h).
 * Use with: -include spinlock.h
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

/* Minimal spinlock_t compatible with kernel definition (volatile int) */
typedef volatile int spinlock_t;

#define SPINLOCK_INIT 0

static inline void spinlock_init(spinlock_t *l)          { *l = 0; }
static inline void spinlock_acquire(spinlock_t *l)       { (void)l; }
static inline void spinlock_release(spinlock_t *l)       { (void)l; }
static inline void spinlock_irqsave_acquire(spinlock_t *l, unsigned long long *f) { (void)l; (void)f; }
static inline void spinlock_irqsave_release(spinlock_t *l, unsigned long long f)  { (void)l; (void)f; }

/* Lockdep stubs called by the kernel's spinlock implementation */
static inline void lockdep_spinlock_acquired(const void *l) { (void)l; }
static inline void lockdep_spinlock_released(const void *l) { (void)l; }
static inline void spinlock_detect_lockup(const volatile void *l) { (void)l; }
static inline void spinlock_register_owner(volatile void *l) { (void)l; }
static inline void spinlock_unregister_owner(volatile void *l) { (void)l; }

/* preempt stubs */
static inline void preempt_disable(void) { }
static inline void preempt_enable(void) { }

/* schedule stub */
static inline void schedule(void) { }

#endif /* SPINLOCK_H */
