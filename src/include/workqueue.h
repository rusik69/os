#ifndef WORKQUEUE_H
#define WORKQUEUE_H

#include "types.h"

/*
 * Workqueue — Deferred work execution with thread pool.
 *
 * Provides two modes:
 *   1. System workqueue (default, single-threaded, backward-compatible)
 *   2. Unbound workqueues with multiple concurrent workers
 *
 * Unbound workers have no CPU affinity — they can run on any available
 * CPU, providing parallel work execution without being pinned to a
 * specific core.
 */

/* ── Constants ───────────────────────────────────────────────────── */

/* Maximum number of work items in a single workqueue */
#define WORKQUEUE_MAX           16

/* Maximum simultaneous unbound workers per workqueue */
#define WQ_UNBOUND_MAX_WORKERS   4

/* Maximum total workqueues */
#define WQ_MAX_WORKQUEUES        8

/* Flags for workqueue_create() */
#define WQ_UNBOUND    (1U << 0)   /* Workers are unbound (any CPU) */
#define WQ_HIGHPRI    (1U << 1)   /* Workers run at RT priority */

/* Work item callback */
typedef void (*work_fn_t)(void *arg);

/* ── Workqueue descriptor ────────────────────────────────────────── */

struct workqueue_struct {
    char      name[24];                       /* human-readable name */
    uint32_t  flags;                          /* WQ_UNBOUND, etc. */
    int       num_workers;                    /* number of worker threads */
    int       workers_running;                /* actually started workers */
};

/* ── API: system workqueue (backward-compatible) ─────────────────── */

/* Schedule a work item on the system workqueue.  Returns work_id (>=0)
 * on success, or -1 if the queue is full.  Thread-safe. */
int workqueue_schedule(work_fn_t fn, void *arg);

/* Wait for all pending work items on the system workqueue to complete.
 * Spins until the queue drains.  Callable from any context that can yield. */
void workqueue_drain(void);

/* ── API: unbound workqueues ─────────────────────────────────────── */

/* Create a named workqueue.  If flags & WQ_UNBOUND, multiple worker
 * threads are spawned (up to WQ_UNBOUND_MAX_WORKERS).  Returns a
 * pointer to the workqueue, or NULL on failure. */
struct workqueue_struct *workqueue_create(const char *name, uint32_t flags);

/* Schedule a work item on a specific workqueue.  Equivalent to
 * workqueue_schedule() but dispatches to @wq instead of the system
 * queue.  Returns 0 on success, -1 if full. */
int workqueue_schedule_on(struct workqueue_struct *wq,
                          work_fn_t fn, void *arg);

/* Flush a workqueue: block until all currently-pending work items
 * have been processed.  New work may still be added concurrently. */
void workqueue_flush(struct workqueue_struct *wq);

/* Destroy a workqueue: flush pending work, terminate workers, free
 * resources.  Do NOT use the workqueue pointer after this call. */
void workqueue_destroy(struct workqueue_struct *wq);

/* ── Initialization ──────────────────────────────────────────────── */

/* Initialize the workqueue subsystem (creates the system workqueue). */
void workqueue_init(void);

#endif /* WORKQUEUE_H */
