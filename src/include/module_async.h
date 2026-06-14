#ifndef MODULE_ASYNC_H
#define MODULE_ASYNC_H

#include "types.h"

/*
 * module_async.h — Module async initialisation API
 *
 * Provides support for deferred module init via workqueues.
 * Modules with the ASYNC_PROBE flag stay in LOADING state until
 * their init function completes asynchronously.
 */

/* Forward declaration */
struct kernel_module;

/* Module flags */
#define MODULE_FLAG_ASYNC_PROBE  (1U << 0)  /* Module uses async init */

/* Async work item — passed to the workqueue handler */
struct module_async_work {
    struct kernel_module *mod;
    int (*entry)(void);
};

/*
 * Mark a module for asynchronous initialisation.
 * The module stays in MODULE_LOADING state until init completes.
 */
int module_async_set_probe(struct kernel_module *mod);

/*
 * Schedule the module's init function on the system workqueue.
 * The work handler calls entry() and transitions the module to LIVE
 * on success.
 */
int module_async_schedule_init(struct kernel_module *mod,
                                int (*entry)(void));

/*
 * Workqueue handler — calls the module init, transitions state.
 * Internal, but exposed for unit testing.
 */
void module_async_work_handler(void *arg);

/*
 * Check if a module is still waiting for async init to complete.
 * Returns 1 if pending, 0 otherwise.
 */
int module_async_is_pending(struct kernel_module *mod);

#endif /* MODULE_ASYNC_H */
