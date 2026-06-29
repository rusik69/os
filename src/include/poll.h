#ifndef POLL_H
#define POLL_H

/*
 * poll.h — Kernel-internal poll infrastructure
 *
 * Provides the foundational data structures for I/O event
 * multiplexing: poll(), select(), and epoll().
 *
 * The core abstraction is the poll_table, which collects
 * wait queue registrations from file/driver poll handlers.
 * Instead of busy-waiting, poll/select/epoll register interest
 * via poll_wait() and then block on the collected waitqueues
 * until events arrive, a signal fires, or a timeout expires.
 *
 * Lifecycle:
 *   1. Allocate a poll_table on the stack (or heap for epoll)
 *   2. poll_init_table() or poll_init_table_inline() to init
 *   3. For each fd, invoke the poll handler which calls
 *      poll_wait() to register its waitqueue
 *   4. If nothing ready, poll_schedule() blocks until events
 *   5. Collect results and return to caller
 *   6. poll_free_table() to release resources
 */

#include "types.h"
#include "waitqueue.h"
#include "errno.h"

/* ── Constants ───────────────────────────────────────────────── */

/* Maximum number of poll entries per poll table.
 * Covers PROCESS_FD_MAX (256) file descriptors plus room for
 * internal registrations from epoll/eventfd/timerfd. */
#define POLL_TABLE_MAX  256

/* ── Forward declarations ───────────────────────────────────── */

struct poll_table;

/*
 * poll_fn_t — callback signature for file/driver poll functions.
 *
 * @file:  opaque pointer to the file or device being polled
 * @pt:    poll_table to register waitqueues with (may be NULL
 *         for a direct non-blocking query)
 *
 * Returns a bitmask of POLL* events (POLLIN | POLLOUT | etc.).
 */
typedef int (*poll_fn_t)(void *file, struct poll_table *pt);

/* ── Core data structures ───────────────────────────────────── */

/*
 * poll_queue_entry — a single wait queue registration.
 *
 * When a poll handler calls poll_wait(), an entry is recorded
 * in the poll_table linking the file's wait_queue to the poll
 * infrastructure.  When that wait_queue fires, the polling
 * process is woken so it can re-check readiness.
 *
 * @wq:   the wait_queue being polled (from file/driver state)
 * @key:  private data for epoll (identifies which epoll_entry
 *        this registration belongs to); NULL for poll/select
 */
struct poll_queue_entry {
    struct wait_queue *wq;
    void              *key;
};

/*
 * poll_table — per-call collection of poll_queue_entry objects.
 *
 * For poll() and select(), this is typically stack-allocated
 * with a local entries buffer using poll_init_table_inline().
 * For epoll, the table persists across calls.
 *
 * @entries:     array of registered wait queue entries
 * @nr_entries:  number of active entries
 * @max_entries: capacity of the entries array
 * @error:       non-zero if an allocation or registration failed
 */
struct poll_table {
    struct poll_queue_entry *entries;
    int                     nr_entries;
    int                     max_entries;
    int                     error;
};

/* ── Initialization and teardown ────────────────────────────── */

/*
 * poll_init_table — allocate and initialize a poll table.
 *
 * Allocates an internal entries array of POLL_TABLE_MAX.
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
int poll_init_table(struct poll_table *pt);

/*
 * poll_init_table_inline — initialize with a caller-provided buffer.
 *
 * Use for stack-allocated poll tables to avoid heap allocation.
 * @buf must have at least @max entries and remain valid until
 * poll_free_table() is called.
 */
static inline void
poll_init_table_inline(struct poll_table *pt,
                       struct poll_queue_entry *buf, int max)
{
    pt->entries    = buf;
    pt->nr_entries = 0;
    pt->max_entries = max;
    pt->error      = 0;
}

/*
 * poll_free_table — release resources held by a poll table.
 *
 * Safe to call on a zero-initialised table (does nothing).
 * After this call, the poll_table must not be used without
 * re-initialising.
 */
void poll_free_table(struct poll_table *pt);

/* ── Registration ───────────────────────────────────────────── */

/*
 * poll_wait — register a wait_queue with a poll table.
 *
 * Called from file/driver poll handlers to indicate that @wq
 * should wake the polling process when I/O becomes possible.
 *
 * @pt:  poll_table from the poll handler's @pt argument
 * @wq:  wait_queue to register
 *
 * Returns 0 on success.
 * Returns -ENOMEM if the table is full (nr_entries >= max_entries).
 * Returns -EINVAL if pt is NULL (silently ignored, as some
 *         callers pass NULL for a non-blocking probe).
 */
int poll_wait(struct poll_table *pt, struct wait_queue *wq);

/* ── Blocking ───────────────────────────────────────────────── */

/*
 * poll_schedule — block until events arrive, signal, or timeout.
 *
 * Sleeps on all registered wait queues.  Returns when:
 *   - any registered wait queue fires (events may be ready)
 *   - a signal is pending for the current process
 *   - the specified timeout expires
 *
 * @pt:          poll table with registered wait queues
 * @timeout_ms:  timeout in milliseconds
 *               (~0ULL = infinite wait)
 *               (0       = non-blocking probe)
 *
 * Returns:  0 if woken (events may be ready — re-check fds)
 *          -EINTR if interrupted by a signal
 *          -ETIME if timeout expired without events
 */
int poll_schedule(struct poll_table *pt, uint64_t timeout_ms);

/* ── Helper inlines ─────────────────────────────────────────── */

/*
 * poll_queue_entry_init — initialise a poll_queue_entry.
 */
static inline void
poll_queue_entry_init(struct poll_queue_entry *e,
                      struct wait_queue *wq)
{
    e->wq  = wq;
    e->key = NULL;
}

/*
 * poll_table_has_error — check if the poll table hit an error.
 */
static inline int
poll_table_has_error(const struct poll_table *pt)
{
    return pt->error != 0;
}

/*
 * poll_table_clear_error — reset the error flag.
 */
static inline void
poll_table_clear_error(struct poll_table *pt)
{
    pt->error = 0;
}

#endif /* POLL_H */
