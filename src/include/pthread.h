#ifndef PTHREAD_H
#define PTHREAD_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Thread types ─────────────────────────────────────────────────── */

typedef uint64_t pthread_t;

/* ── Thread attributes (minimal) ──────────────────────────────────── */

typedef struct {
    int          detached;
    size_t       stack_size;
    void        *stack_addr;
} pthread_attr_t;

#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1

/* Default stack size (64 KB) */
#define PTHREAD_STACK_MIN        (8 * 4096)
#define PTHREAD_STACK_DEFAULT    (16 * 4096)

/* ── Mutex types ──────────────────────────────────────────────────── */

typedef struct {
    volatile int  lock;       /* 0 = unlocked, 1 = locked (no waiters), 2 = locked (waiters) */
    int           type;       /* NORMAL, RECURSIVE, ERRORCHECK */
    int           recursive_count;
    pthread_t     owner;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER  { 0, 0, 0, 0 }
#define PTHREAD_MUTEX_NORMAL       0
#define PTHREAD_MUTEX_RECURSIVE    1
#define PTHREAD_MUTEX_ERRORCHECK   2

typedef struct {
    int protocol;
    int prioceiling;
} pthread_mutexattr_t;

/* ── Condition variable types ─────────────────────────────────────── */

typedef struct {
    volatile int  waiters;    /* number of waiters (for wake-all) */
    volatile int  signal_cnt; /* futex value for wake/signal */
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER  { 0, 0 }

typedef struct {
    int clockid;
} pthread_condattr_t;

/* ── Once / key / join helpers ────────────────────────────────────── */

typedef int pthread_once_t;
#define PTHREAD_ONCE_INIT  0

/* ── Thread functions ─────────────────────────────────────────────── */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_equal(pthread_t t1, pthread_t t2);

/* ── Mutex functions ──────────────────────────────────────────────── */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

/* ── Condition variable functions ─────────────────────────────────── */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime);

/* ── Once functions ───────────────────────────────────────────────── */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

/* ── Attribute functions ──────────────────────────────────────────── */

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize);
int pthread_attr_setstackaddr(pthread_attr_t *attr, void *stackaddr);
int pthread_attr_getstackaddr(const pthread_attr_t *attr, void **stackaddr);

#ifdef __cplusplus
}
#endif

#endif /* PTHREAD_H */
