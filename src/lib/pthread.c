/*
 * pthread.c — Minimal POSIX threads library (Item U18)
 *
 * ── Architecture Overview ──────────────────────────────────────────
 *
 * This file provides a minimal implementation of the POSIX threads (pthreads)
 * API, callable from both ring-0 (built-in shell commands compiled into the
 * kernel binary) and ring-3 (user-space ELF programs).  The syscall dispatch
 * layer (SYS_THREAD_CREATE / JOIN / EXIT, SYS_FUTEX, SYS_GETTID) handles
 * both paths transparently.
 *
 * The implementation depends on the kernel's futex syscall for all
 * synchronization primitives.  No spinlocks or interrupt-disabling are used
 * by this layer — blocking synchronisation is always mediated by the kernel's
 * futex wait/wake mechanism.
 *
 * ── Components ─────────────────────────────────────────────────────
 *
 * Thread management    — pthread_create/join/exit/self/equal
 * Thread attributes    — pthread_attr_{init,destroy,set/get}*
 * Mutex                — FUTEX-based; supports NORMAL, RECURSIVE, ERRORCHECK
 * Condition variables  — FUTEX-based; signal/broadcast with signal_cnt
 * Barriers             — Simple counter-based generation barrier
 * RW locks             — Readers-writer lock (no priority)
 * Keys (TLS)           — Static linear-scan key allocator
 * pthread_once         — Simple test-and-set
 *
 * ── Mutex States ───────────────────────────────────────────────────
 *
 * Each mutex uses a single 32-bit futex word with three states:
 *   0 = unlocked
 *   1 = locked, no waiters (fast path, no syscall on unlock)
 *   2 = locked, waiters present (slow path, FUTEX_WAKE on unlock)
 *
 * Locking algorithm (pthread_mutex_lock):
 *   1. Fast path: cmpxchg(0→1).  Acquired if old==0.
 *   2. Slow path loop:
 *      a. cmpxchg(0→2) — catch any concurrent unlock between 0→1 try and here
 *      b. atomic_xchg(1→2) — ensure waiter flag is set
 *      c. FUTEX_WAIT(&lock, 2) — sleep until kernel wakes us
 *
 * Unlocking (pthread_mutex_unlock):
 *   1. atomic_xchg(&lock, 0) — release and read old state atomically
 *   2. If old state was 2 (had waiters), FUTEX_WAKE one waiter.
 *      If 1 (no waiters), no syscall needed — the single waiter spinning
 *      on step 2a will succeed.
 *
 * ── Condition Variables ────────────────────────────────────────────
 *
 * Condition variables use a monotonically-increasing signal_cnt as the
 * futex word.  pthread_cond_signal increments signal_cnt and wakes one
 * waiter.  pthread_cond_broadcast increments and wakes all.  pthread_cond_wait
 * saves the current signal_cnt, releases the mutex, then FUTEX_WAITs on
 * signal_cnt until it changes (meaning a signal or broadcast happened).
 *
 * This pattern avoids lost-wakeup races: the waiter is guaranteed to see
 * either the signal_cnt change (if a signal occurs after the save) or the
 * unlocked mutex state (if the release completes before any signal).
 *
 * ── Barriers ───────────────────────────────────────────────────────
 *
 * Barrier state is stored as a simple array of three ints:
 *   [0] total count    [1] remaining count    [2] futex word / generation
 * The last thread to arrive resets remaining to total, increments the
 * generation, and wakes all waiting threads.
 *
 * ── RW Locks ───────────────────────────────────────────────────────
 *
 * Readers-writer locks track [readers, writer_flag] as separate ints.
 * Writers spin on the writer_flag using test-and-set + FUTEX_WAIT.
 * Readers increment count optimistically, then back off if a writer
 * snuck in between the check and the increment.  This is a simple
 * writer-preference-free implementation.
 *
 * ── Thread Keys (TLS) ──────────────────────────────────────────────
 *
 * Thread-specific data uses a static linear-scan key allocator with a
 * fixed maximum of PTHREAD_KEYS_MAX (128) keys.  The global storage array
 * pthread_key_values[] holds values for all keys — this is NOT per-thread
 * storage.  See limitations below.
 *
 * ── Known Limitations ──────────────────────────────────────────────
 *
 * 1. No true per-thread TLS:   pthread_key_values[] is a global array,
 *    shared across all threads.  pthread_getspecific / setspecific
 *    operate on a single global store.  True __thread or __declspec(thread)
 *    TLS requires per-thread data structures (e.g. an array of arrays,
 *    indexed by tid) which are not yet implemented.
 *
 * 2. No cancellation:   pthread_cancel, pthread_testcancel, and
 *    cleanup handlers are not implemented.  Threads run until they
 *    call pthread_exit or the process exits.
 *
 * 3. No detached thread cleanup:   Detached threads (PTHREAD_CREATE_DETACHED)
 *    are accepted by pthread_attr_setdetachstate but the attribute is
 *    ignored at pthread_create time — all threads are joinable.  There
 *    is no thread reaper / destructor callback for detached threads.
 *
 * 4. No spinlock or rwlock attributes:   pthread_rwlockattr_t and
 *    pthread_spinlock_t are not implemented.  The rwlock functions are
 *    defined but are static (not yet exposed in the public headers).
 *
 * 5. No priority inheritance:   Mutexes do not support
 *    PTHREAD_PRIO_INHERIT or PTHREAD_PRIO_PROTECT.  Priority inversion
 *    is possible.
 *
 * 6. No robust mutexes:   PTHREAD_MUTEX_ROBUST is not supported.  If a
 *    thread holding a mutex terminates without unlocking, waiters will
 *    deadlock.
 *
 * 7. No thread scheduling attributes:   pthread_attr_setschedparam,
 *    pthread_attr_setschedpolicy, pthread_attr_setscope have no effect.
 *    All threads share the kernel's default scheduling policy.
 *
 * 8. CPU affinity not supported:   pthread_setaffinity_np and
 *    pthread_getaffinity_np are not implemented.
 *
 * 9. Barrier functions are static:   pthread_barrier_{init,wait,destroy}
 *    are defined as static and not yet linked to the public API,
 *    pending header exposure.
 *
 * 10. Keys not per-thread:   The pthread_key API uses a single global
 *     storage array.  All threads share the same value for any given
 *     key.  See limitation #1.
 *
 * 11. No name or stack guard pages:   pthread_setname_np and guard-page
 *     support in stack allocation are not implemented.
 *
 * 12. Futex syscall errors not relayed:   Some fast-path futex wake
 *     failures are silently ignored.  Robustness could be improved.
 */

#include "pthread.h"
#include "syscall.h"
#include "libc.h"
#include "errno.h"
#include "string.h"

/* ── Fast-path: direct kernel calls when running in ring 0 ───────── */
/* If we're running in kernel mode, we can bypass the syscall overhead
 * and call the kernel functions directly.  The linker resolves these
 * because libc is compiled into the kernel binary. */
#ifdef KERNEL_INTERNAL
/* We compile as part of kernel, but KERNEL_INTERNAL might not be defined
 * for libc files. Use the syscall path for consistency. */
#endif

/* ── Internal helpers ────────────────────────────────────────────── */

/* Raw futex syscall wrapper */
static inline int futex(int *uaddr, int op, int val,
                        const struct timespec *timeout,
                        int *uaddr2)
{
    return (int)libc_syscall(SYS_FUTEX,
                             (uint64_t)(uintptr_t)uaddr,
                             (uint64_t)op,
                             (uint64_t)val,
                             (uint64_t)(uintptr_t)timeout,
                             (uint64_t)(uintptr_t)uaddr2);
}

#define FUTEX_WAIT      0
#define FUTEX_WAKE      1
#define FUTEX_WAKE_OP   5
#define FUTEX_CMP_REQUEUE  4

/* ── Thread management ──────────────────────────────────────────── */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    if (!thread || !start_routine)
        return EINVAL;

    /* For now we ignore attr — use default stack and non-detached. */
    (void)attr;

    int ret = (int)libc_syscall(SYS_THREAD_CREATE,
                                (uint64_t)(uintptr_t)start_routine,
                                (uint64_t)(uintptr_t)arg,
                                0, 0, 0);

    if (ret < 0)
        return EAGAIN;

    *thread = (pthread_t)(uintptr_t)ret;
    return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
    if (thread == 0)
        return EINVAL;

    int ret = (int)libc_syscall(SYS_THREAD_JOIN,
                                (uint64_t)thread,
                                (uint64_t)(uintptr_t)retval,
                                0, 0, 0);

    return (ret < 0) ? EINVAL : 0;
}

void pthread_exit(void *retval)
{
    libc_syscall(SYS_THREAD_EXIT, (uint64_t)(uintptr_t)retval,
                 0, 0, 0, 0);
    /* Never reached */
    for (;;);
}

pthread_t pthread_self(void)
{
    return (pthread_t)libc_syscall(SYS_GETTID, 0, 0, 0, 0, 0);
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
    return (t1 == t2);
}

/* ── pthread_once ───────────────────────────────────────────────── */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    if (!once_control || !init_routine)
        return EINVAL;

    /* Simple test-and-set using the volatile int */
    if (*once_control == 0) {
        *once_control = 1;
        init_routine();
    }
    return 0;
}

/* ── Thread attributes ──────────────────────────────────────────── */

int pthread_attr_init(pthread_attr_t *attr)
{
    if (!attr) return EINVAL;
    memset(attr, 0, sizeof(*attr));
    attr->detached    = PTHREAD_CREATE_JOINABLE;
    attr->stack_size  = PTHREAD_STACK_DEFAULT;
    attr->stack_addr  = NULL;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
    if (!attr) return EINVAL;
    if (detachstate != PTHREAD_CREATE_JOINABLE &&
        detachstate != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    attr->detached = detachstate;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
    if (!attr || !detachstate) return EINVAL;
    *detachstate = attr->detached;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
    if (!attr) return EINVAL;
    if (stacksize < PTHREAD_STACK_MIN)
        return EINVAL;
    attr->stack_size = stacksize;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
    if (!attr || !stacksize) return EINVAL;
    *stacksize = attr->stack_size;
    return 0;
}

int pthread_attr_setstackaddr(pthread_attr_t *attr, void *stackaddr)
{
    if (!attr) return EINVAL;
    attr->stack_addr = stackaddr;
    return 0;
}

int pthread_attr_getstackaddr(const pthread_attr_t *attr, void **stackaddr)
{
    if (!attr || !stackaddr) return EINVAL;
    *stackaddr = attr->stack_addr;
    return 0;
}

/* ── Mutex implementation (futex-based) ─────────────────────────── */
/*
 * Lock states:
 *   0 = unlocked
 *   1 = locked, no contention (no waiters)
 *   2 = locked, contention (waiters in kernel futex queue)
 */

/* Atomically compare-and-swap a 32-bit value.
 * Returns old value. */
static inline int cmpxchg(volatile int *ptr, int expected, int desired)
{
    int old;
    __asm__ volatile(
        "lock cmpxchg %2, %1"
        : "=a"(old), "+m"(*ptr)
        : "r"(desired), "a"(expected)
        : "memory");
    return old;
}

/* Atomically exchange a 32-bit value. Returns old value. */
static inline int atomic_xchg(volatile int *ptr, int val)
{
    int old;
    __asm__ volatile(
        "xchg %0, %1"
        : "=r"(old), "+m"(*ptr)
        : "0"(val)
        : "memory");
    return old;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    if (!mutex) return EINVAL;

    mutex->lock           = 0;
    mutex->type           = 0;  /* PTHREAD_MUTEX_NORMAL (default; attr handling is a TODO) */
    mutex->recursive_count = 0;
    mutex->owner          = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;
    /* Check if anyone is still waiting */
    if (mutex->lock != 0)
        return EBUSY;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;

    pthread_t self = pthread_self();

    /* Handle recursive mutex */
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->owner == self) {
        mutex->recursive_count++;
        return 0;
    }

    /* Handle errorcheck mutex */
    if (mutex->type == PTHREAD_MUTEX_ERRORCHECK && mutex->owner == self) {
        return EDEADLK;
    }

    /* Fast path: try to acquire, state 0→1 */
    if (cmpxchg(&mutex->lock, 0, 1) == 0) {
        mutex->owner = self;
        mutex->recursive_count = 1;
        return 0;
    }

    /* Contention path: try to acquire with waiter flag, then futex wait */
    for (;;) {
        /* Try 0→2 (unlocked → locked-with-waiters) - if unlocked now, grab it */
        if (cmpxchg(&mutex->lock, 0, 2) == 0) {
            mutex->owner = self;
            mutex->recursive_count = 1;
            return 0;
        }

        /* Try 1→2 (locked → locked-with-waiters) to ensure waiter flag is set */
        int prev = atomic_xchg(&mutex->lock, 2);
        if (prev == 0) {
            /* We got lucky and it was unlocked */
            mutex->owner = self;
            mutex->recursive_count = 1;
            return 0;
        }

        /* Sleep on the futex (state == 2) */
        futex((int *)(uintptr_t)&mutex->lock, FUTEX_WAIT, 2, NULL, NULL);
        /* After wake, retry */
    }
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;

    pthread_t self = pthread_self();

    /* Handle recursive mutex */
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->owner == self) {
        mutex->recursive_count++;
        return 0;
    }

    /* Handle errorcheck mutex */
    if (mutex->type == PTHREAD_MUTEX_ERRORCHECK && mutex->owner == self) {
        return EDEADLK;
    }

    /* Try to acquire, state 0→1 */
    if (cmpxchg(&mutex->lock, 0, 1) == 0) {
        mutex->owner = self;
        mutex->recursive_count = 1;
        return 0;
    }

    return EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;

    pthread_t self = pthread_self();

    /* Check ownership for errorcheck and recursive mutexes */
    if (mutex->type != PTHREAD_MUTEX_NORMAL && mutex->owner != self)
        return EPERM;

    /* Handle recursive mutex */
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        mutex->recursive_count--;
        if (mutex->recursive_count > 0)
            return 0;
    }

    mutex->owner = 0;

    /* Release: state 2→0 (with potential waiters), or 1→0 (no waiters).
     * If state was 2 (waiters), we need to FUTEX_WAKE one. */
    int prev = atomic_xchg(&mutex->lock, 0);
    if (prev == 2) {
        /* There may be waiters — wake one */
        futex((int *)(uintptr_t)&mutex->lock, FUTEX_WAKE, 1, NULL, NULL);
    }

    return 0;
}

/* ── Condition variable implementation (futex-based) ────────────── */
/*
 * Simple condvar: signal_cnt is the futex word.
 *   pthread_cond_signal: increment signal_cnt, FUTEX_WAKE(1)
 *   pthread_cond_broadcast: increment signal_cnt, FUTEX_WAKE(INT_MAX)
 *   pthread_cond_wait: atomically release mutex, FUTEX_WAIT on signal_cnt,
 *                      then re-acquire mutex
 */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;
    (void)attr;
    cond->waiters    = 0;
    cond->signal_cnt = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    if (!cond) return EINVAL;
    if (cond->waiters > 0)
        return EBUSY;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    if (!cond || !mutex) return EINVAL;

    /* Read the current signal count before releasing the mutex */
    int signal_cnt = cond->signal_cnt;

    /* Increment waiter count */
    __sync_fetch_and_add(&cond->waiters, 1);

    /* Release the mutex */
    int unlock_ret = pthread_mutex_unlock(mutex);
    if (unlock_ret != 0) {
        __sync_fetch_and_sub(&cond->waiters, 1);
        return unlock_ret;
    }

    /* Wait for signal/broadcast */
    futex((int *)(uintptr_t)&cond->signal_cnt, FUTEX_WAIT, signal_cnt, NULL, NULL);

    /* Decrement waiter count */
    __sync_fetch_and_sub(&cond->waiters, 1);

    /* Re-acquire the mutex */
    return pthread_mutex_lock(mutex);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime)
{
    if (!cond || !mutex || !abstime) return EINVAL;

    int signal_cnt = cond->signal_cnt;

    __sync_fetch_and_add(&cond->waiters, 1);

    int unlock_ret = pthread_mutex_unlock(mutex);
    if (unlock_ret != 0) {
        __sync_fetch_and_sub(&cond->waiters, 1);
        return unlock_ret;
    }

    int ret = futex((int *)(uintptr_t)&cond->signal_cnt, FUTEX_WAIT, signal_cnt,
                    abstime, NULL);

    __sync_fetch_and_sub(&cond->waiters, 1);

    /* Re-acquire the mutex */
    int lock_ret = pthread_mutex_lock(mutex);
    if (lock_ret != 0)
        return lock_ret;

    return (ret == 0) ? 0 : ETIMEDOUT;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    if (!cond) return EINVAL;

    /* Increment signal count so FUTEX_WAITers see the change */
    __sync_fetch_and_add(&cond->signal_cnt, 1);

    /* Wake one waiter */
    futex((int *)(uintptr_t)&cond->signal_cnt, FUTEX_WAKE, 1, NULL, NULL);

    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (!cond) return EINVAL;

    /* Increment signal count */
    __sync_fetch_and_add(&cond->signal_cnt, 1);

    /* Wake all waiters */
    futex((int *)(uintptr_t)&cond->signal_cnt, FUTEX_WAKE, 2147483647, NULL, NULL);

    return 0;
}

/* ── pthread_barrier_init ─────────────────────────────── */
static int pthread_barrier_init(void *barrier, const void *attr, unsigned int count)
{
    if (!barrier || count == 0) return EINVAL;
    (void)attr;
    /* Simple barrier struct: [count, remaining, futex_word] */
    volatile int *b = (volatile int *)barrier;
    b[0] = (int)count;    /* total count */
    b[1] = (int)count;    /* remaining */
    b[2] = 0;             /* futex word / generation */
    return 0;
}
/* ── pthread_barrier_wait ─────────────────────────────── */
static int pthread_barrier_wait(void *barrier)
{
    if (!barrier) return EINVAL;
    volatile int *b = (volatile int *)barrier;
    int count = b[0];

    int rem = __sync_fetch_and_sub(&b[1], 1);
    if (rem == 1) {
        /* Last thread to arrive — reset and wake everyone */
        b[1] = count;
        __sync_fetch_and_add(&b[2], 1);
        futex((int *)(uintptr_t)&b[2], FUTEX_WAKE, 2147483647, NULL, NULL);
        return 1; /* PTHREAD_BARRIER_SERIAL_THREAD */
    }
    /* Wait for generation change */
    int gen = b[2];
    do {
        futex((int *)(uintptr_t)&b[2], FUTEX_WAIT, gen, NULL, NULL);
    } while (b[2] == gen);

    return 0;
}
/* ── pthread_barrier_destroy ─────────────────────────────── */
static int pthread_barrier_destroy(void *barrier)
{
    if (!barrier) return EINVAL;
    volatile int *b = (volatile int *)barrier;
    if (b[1] != b[0])
        return EBUSY;
    return 0;
}
/* ── pthread_rwlock_init ─────────────────────────────── */
static int pthread_rwlock_init(void *rwlock, const void *attr)
{
    if (!rwlock) return EINVAL;
    (void)attr;
    volatile int *r = (volatile int *)rwlock;
    r[0] = 0; /* readers count */
    r[1] = 0; /* writer flag */
    return 0;
}
/* ── pthread_rwlock_destroy ─────────────────────────────── */
static int pthread_rwlock_destroy(void *rwlock)
{
    if (!rwlock) return EINVAL;
    return 0;
}
/* ── pthread_rwlock_rdlock ─────────────────────────────── */
static int pthread_rwlock_rdlock(void *rwlock)
{
    if (!rwlock) return EINVAL;
    volatile int *r = (volatile int *)rwlock;
    for (;;) {
        /* Wait until no writer */
        while (r[1])
            futex((int *)(uintptr_t)&r[1], FUTEX_WAIT, 1, NULL, NULL);
        __sync_fetch_and_add(&r[0], 1);
        if (!r[1])
            return 0;
        /* Writer started before we incremented — back off */
        __sync_fetch_and_sub(&r[0], 1);
    }
}
/* ── pthread_rwlock_wrlock ─────────────────────────────── */
static int pthread_rwlock_wrlock(void *rwlock)
{
    if (!rwlock) return EINVAL;
    volatile int *r = (volatile int *)rwlock;
    while (__sync_lock_test_and_set(&r[1], 1))
        futex((int *)(uintptr_t)&r[1], FUTEX_WAIT, 1, NULL, NULL);
    /* Wait for readers to finish */
    while (r[0])
        futex((int *)(uintptr_t)&r[0], FUTEX_WAIT, 0, NULL, NULL);
    return 0;
}
/* ── pthread_rwlock_unlock ─────────────────────────────── */
static int pthread_rwlock_unlock(void *rwlock)
{
    if (!rwlock) return EINVAL;
    volatile int *r = (volatile int *)rwlock;
    if (r[1]) {
        /* Writer unlock */
        r[1] = 0;
        futex((int *)(uintptr_t)&r[1], FUTEX_WAKE, 2147483647, NULL, NULL);
    } else if (r[0] > 0) {
        /* Reader unlock */
        int rem = __sync_fetch_and_sub(&r[0], 1);
        if (rem == 1) {
            /* Last reader — wake writers */
            futex((int *)(uintptr_t)&r[0], FUTEX_WAKE, 1, NULL, NULL);
        }
    }
    return 0;
}
/* ── Key management (simple static array) ─────────────────────────────── */
#define PTHREAD_KEYS_MAX 128
static volatile int pthread_key_slots[PTHREAD_KEYS_MAX];
/* TLS value storage per key — very simplified, per-thread not implemented */
static void *pthread_key_values[PTHREAD_KEYS_MAX];
static volatile int pthread_key_next = 0;

/* ── pthread_key_create ─────────────────────────────── */
static int pthread_key_create(void *key, void *destructor)
{
    if (!key) return EINVAL;
    (void)destructor;
    for (int i = pthread_key_next; i < PTHREAD_KEYS_MAX; i++) {
        if (!pthread_key_slots[i]) {
            pthread_key_slots[i] = 1;
            pthread_key_values[i] = NULL;
            *(int *)key = i;
            if (i + 1 > pthread_key_next)
                pthread_key_next = i + 1;
            return 0;
        }
    }
    return EAGAIN;
}
/* ── pthread_key_delete ─────────────────────────────── */
static int pthread_key_delete(void *key)
{
    int idx = *(int *)key;
    if (idx < 0 || idx >= PTHREAD_KEYS_MAX || !pthread_key_slots[idx])
        return EINVAL;
    pthread_key_slots[idx] = 0;
    pthread_key_values[idx] = NULL;
    return 0;
}
/* ── pthread_setspecific ─────────────────────────────── */
static int pthread_setspecific(void *key, const void *value)
{
    int idx = *(int *)key;
    if (idx < 0 || idx >= PTHREAD_KEYS_MAX || !pthread_key_slots[idx])
        return EINVAL;
    pthread_key_values[idx] = (void *)(uintptr_t)value;
    return 0;
}
/* ── pthread_getspecific ─────────────────────────────── */
static void* pthread_getspecific(void *key)
{
    int idx = *(int *)key;
    if (idx < 0 || idx >= PTHREAD_KEYS_MAX || !pthread_key_slots[idx])
        return NULL;
    return pthread_key_values[idx];
}
