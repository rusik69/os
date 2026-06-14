/*
 * module_async.c — Module async initialisation
 *
 * Supports ASYNC_PROBE flag on kernel modules. When a module is loaded
 * with this flag, its init function is deferred to a workqueue so that
 * the module loader can return immediately without waiting for the
 * module's init to complete (useful for slow device probes).
 *
 * The module stays in the MODULE_LOADING state until the async init
 * completes.  user-space can observe the state via sysfs or
 * sys_query_module (shows "loading-async").
 */

#define KERNEL_INTERNAL
#include "module.h"
#include "module_async.h"
#include "printf.h"
#include "string.h"
#include "scheduler.h"
#include "workqueue.h"
#include "errno.h"

/* ── ASYNC_PROBE flag ─────────────────────────────────────────────── */

/*
 * Mark a module as having an async init.  This sets the ASYNC_PROBE flag
 * on the module and transitions it to the MODULE_LOADING state if it
 * hasn't already been set live.
 *
 * The actual init function must be scheduled separately via
 * module_async_schedule_init().
 *
 * Returns 0 on success, -EINVAL if mod is NULL.
 */
int module_async_set_probe(struct kernel_module *mod)
{
    if (!mod)
        return -EINVAL;

    /* Set the ASYNC_PROBE flag.  We keep the module in LOADING state
     * until the async init completes. */
    mod->flags |= MODULE_FLAG_ASYNC_PROBE;
    mod->state = MODULE_LOADING;

    kprintf("[MOD_ASYNC] Module '%s' set for async init\\n", mod->name);
    return 0;
}

/*
 * Schedule the module's init function to run on the system workqueue.
 * The module must already have the ASYNC_PROBE flag set.
 *
 * @mod:  The module to initialise asynchronously.
 * @entry: The module's init entry function to call.
 *
 * Returns 0 on success, -EINVAL on invalid args.
 */
int module_async_schedule_init(struct kernel_module *mod,
                                int (*entry)(void))
{
    if (!mod || !entry)
        return -EINVAL;

    if (!(mod->flags & MODULE_FLAG_ASYNC_PROBE)) {
        kprintf("[MOD_ASYNC] Warning: '%s' not flagged for async init\\n",
                mod->name);
    }

    /* Schedule on system workqueue.  The work callback will call
     * the module's init function and then mark the module LIVE. */
    struct module_async_work *aw = NULL;

    /* Allocate and populate the work context */
    aw = (struct module_async_work *)kmalloc(sizeof(*aw));
    if (!aw)
        return -ENOMEM;

    aw->mod = mod;
    aw->entry = entry;

    /* The work function is called by the workqueue thread */
    int work_id = workqueue_schedule(module_async_work_handler, aw);
    if (work_id < 0) {
        kfree(aw);
        return -EAGAIN;
    }

    kprintf("[MOD_ASYNC] Scheduled async init for '%s' (work_id=%d)\\n",
            mod->name, work_id);
    return 0;
}

/*
 * Workqueue handler for async module init.
 * Called in workqueue context.  Calls the module's init function and
 * transitions the module to LIVE on success, or ERROR on failure.
 */
void module_async_work_handler(void *arg)
{
    struct module_async_work *aw = (struct module_async_work *)arg;
    if (!aw || !aw->mod || !aw->entry) {
        kprintf("[MOD_ASYNC] Invalid async work item\\n");
        kfree(aw);
        return;
    }

    struct kernel_module *mod = aw->mod;
    int (*entry)(void) = aw->entry;

    kprintf("[MOD_ASYNC] Running async init for '%s'...\\n", mod->name);

    int ret = entry();

    if (ret == 0) {
        mod->state = MODULE_LIVE;
        mod->flags &= ~(uint32_t)MODULE_FLAG_ASYNC_PROBE;
        kprintf("[MOD_ASYNC] Async init for '%s' completed successfully\\n",
                mod->name);

        /* Apply any boot-time cmdline parameters now that module is live */
        module_apply_cmdline_params(mod);

        /* Create sysfs entries for parameters */
        module_sysfs_add_params(mod);
    } else {
        mod->state = MODULE_ERROR;
        kprintf("[MOD_ASYNC] ERROR: Async init for '%s' returned %d\\n",
                mod->name, ret);
    }

    kfree(aw);
}

/*
 * Check if a module is still in async init (LOADING with ASYNC_PROBE).
 * Returns 1 if the module is still initialising asynchronously, 0 otherwise.
 */
int module_async_is_pending(struct kernel_module *mod)
{
    if (!mod)
        return 0;
    return (mod->state == MODULE_LOADING &&
            (mod->flags & MODULE_FLAG_ASYNC_PROBE)) ? 1 : 0;
}
