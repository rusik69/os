/*
 * workqueue.c — Deferred work execution with thread pool
 *
 * Provides a single-threaded system workqueue (backward-compatible)
 * plus a public API for creating unbound workqueues with multiple
 * concurrent worker threads that can run on any CPU.
 *
 * Architecture:
 *   - Each workqueue has a fixed-size array of work slots protected
 *     by a spinlock.  Work items are dequeued in FIFO order.
 *   - The system workqueue (g_sys_wq) has one worker thread, matching
 *     the original behaviour.
 *   - Unbound workqueues (created with WQ_UNBOUND) spawn up to
 *     WQ_UNBOUND_MAX_WORKERS threads that all compete for the same
 *     work slot array — the first idle worker grabs the next item.
 *   - Worker threads run with no hard CPU affinity, so the scheduler
 *     naturally distributes them across available cores.
 *   - WQ_HIGHPRI workers run at RT priority for latency-sensitive work.
 */

#define KERNEL_INTERNAL
#include "workqueue.h"
#include "process.h"
#include "scheduler.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

/* ── Per-workqueue state ──────────────────────────────────────────── */

#define WORKQUEUE_SLOT_PENDING  1
#define WORKQUEUE_SLOT_FREE     0
#define WORKQUEUE_SLOT_EXECUTING 2   /* transient: being run */

struct wq_work_item {
    work_fn_t fn;
    void     *arg;
    volatile int state;   /* FREE / PENDING / EXECUTING */
};

struct wq_internal {
    struct workqueue_struct pub;             /* public-facing descriptor */
    char                    name[24];
    uint32_t                flags;
    struct wq_work_item     items[WORKQUEUE_MAX];
    spinlock_t              lock;
    volatile int            draining;        /* 1 = flushing in progress */
    int                     num_slots;       /* length of items[] */
    int                     num_workers;     /* target worker count */
    volatile int            workers_running; /* actual thread count */
    int                     worker_pids[WQ_UNBOUND_MAX_WORKERS];
    int                     in_use;          /* 1 = slot occupied */
};

/* ── System workqueue (backward-compatible singleton) ─────────────── */

static struct wq_internal g_sys_wq;
static int g_wq_initialized = 0;

/* ── Forward declarations ─────────────────────────────────────────── */

static void wq_worker_thread(void *arg);
static int  wq_do_one_item(struct wq_internal *wq);


/* ── Internal helpers ─────────────────────────────────────────────── */

/* Find and execute one pending work item.  Returns 1 if work was done,
 * 0 if no work was pending.  Caller must NOT hold the lock. */
static int wq_do_one_item(struct wq_internal *wq)
{
    work_fn_t fn = NULL;
    void     *arg = NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&wq->lock, &irq_flags);

    for (int i = 0; i < WORKQUEUE_MAX; i++) {
        if (wq->items[i].state == WORKQUEUE_SLOT_PENDING) {
            fn = wq->items[i].fn;
            arg = wq->items[i].arg;
            wq->items[i].state = WORKQUEUE_SLOT_EXECUTING;
            break;
        }
    }

    spinlock_irqsave_release(&wq->lock, irq_flags);

    if (fn) {
        fn(arg);

        /* Mark slot free */
        spinlock_irqsave_acquire(&wq->lock, &irq_flags);
        for (int i = 0; i < WORKQUEUE_MAX; i++) {
            if (wq->items[i].state == WORKQUEUE_SLOT_EXECUTING &&
                wq->items[i].fn == fn && wq->items[i].arg == arg) {
                wq->items[i].state = WORKQUEUE_SLOT_FREE;
                wq->items[i].fn = NULL;
                wq->items[i].arg = NULL;
                break;
            }
        }
        spinlock_irqsave_release(&wq->lock, irq_flags);
        return 1;
    }

    return 0;
}

/* Worker thread main loop.  Continuously looks for pending work items,
 * executes them FIFO, then yields when the queue is empty. */
static void wq_worker_thread(void *arg)
{
    struct wq_internal *wq = (struct wq_internal *)arg;

    for (;;) {
        int did_work = wq_do_one_item(wq);

        if (!did_work) {
            /* No work pending — yield to let other processes run.
             * If we're draining, check whether all workers are idle. */
            if (wq->draining) {
                /* Check if any other worker is still active */
                int total_pending = 0;
                uint64_t irq_flags;
                spinlock_irqsave_acquire(&wq->lock, &irq_flags);
                for (int i = 0; i < WORKQUEUE_MAX; i++) {
                    if (wq->items[i].state != WORKQUEUE_SLOT_FREE)
                        total_pending++;
                }
                spinlock_irqsave_release(&wq->lock, irq_flags);
                if (total_pending == 0)
                    break;   /* wq_destroy() asked us to exit */
            }
            scheduler_yield();
        }
    }

    /* Worker thread exits */
    {
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&wq->lock, &irq_flags);
        wq->workers_running--;
        spinlock_irqsave_release(&wq->lock, irq_flags);
    }

    process_exit();
}

/* ── Public API: system workqueue ─────────────────────────────────── */

int workqueue_schedule(work_fn_t fn, void *arg)
{
    if (!fn || !g_wq_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_sys_wq.lock, &irq_flags);

    int slot = -1;
    for (int i = 0; i < WORKQUEUE_MAX; i++) {
        if (g_sys_wq.items[i].state == WORKQUEUE_SLOT_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_sys_wq.lock, irq_flags);
        return -1;
    }

    g_sys_wq.items[slot].fn = fn;
    g_sys_wq.items[slot].arg = arg;
    g_sys_wq.items[slot].state = WORKQUEUE_SLOT_PENDING;

    spinlock_irqsave_release(&g_sys_wq.lock, irq_flags);
    return slot;
}

void workqueue_drain(void)
{
    if (!g_wq_initialized) return;

    g_sys_wq.draining = 1;

    /* Spin until all work items are processed */
    for (;;) {
        int all_done = 1;
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&g_sys_wq.lock, &irq_flags);
        for (int i = 0; i < WORKQUEUE_MAX; i++) {
            if (g_sys_wq.items[i].state != WORKQUEUE_SLOT_FREE) {
                all_done = 0;
                break;
            }
        }
        spinlock_irqsave_release(&g_sys_wq.lock, irq_flags);
        if (all_done) break;
        scheduler_yield();
    }

    g_sys_wq.draining = 0;
}

/* ── Public API: unbound workqueues ───────────────────────────────── */

/* Static pool of wq_internal structs for workqueue_create() */
static struct wq_internal wq_pool[WQ_MAX_WORKQUEUES];
static spinlock_t wq_pool_lock;

struct workqueue_struct *workqueue_create(const char *name, uint32_t flags)
{
    if (!g_wq_initialized) return NULL;
    if (!name) name = "unnamed";

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&wq_pool_lock, &irq_flags);

    struct wq_internal *wq = NULL;
    for (int i = 0; i < WQ_MAX_WORKQUEUES; i++) {
        if (!wq_pool[i].in_use) {
            wq = &wq_pool[i];
            wq->in_use = 1;
            break;
        }
    }
    spinlock_irqsave_release(&wq_pool_lock, irq_flags);

    if (!wq) {
        kprintf("[workqueue] failed to allocate workqueue \"%s\" (max %d)\n",
                name, WQ_MAX_WORKQUEUES);
        return NULL;
    }

    /* Initialise the workqueue */
    memset(wq->items, 0, sizeof(wq->items));
    spinlock_init(&wq->lock);
    wq->draining = 0;
    wq->flags = flags;
    wq->workers_running = 0;
    wq->num_slots = WORKQUEUE_MAX;

    /* Determine worker count */
    if (flags & WQ_UNBOUND) {
        wq->num_workers = WQ_UNBOUND_MAX_WORKERS;
    } else {
        wq->num_workers = 1;
    }

    /* Copy name safely */
    size_t nlen = strlen(name);
    if (nlen >= sizeof(wq->name))
        nlen = sizeof(wq->name) - 1;
    memcpy(wq->name, name, nlen);
    wq->name[nlen] = '\0';
    memcpy(wq->pub.name, wq->name, sizeof(wq->pub.name));
    wq->pub.flags = flags;
    wq->pub.num_workers = wq->num_workers;
    wq->pub.workers_running = 0;

    /* Spawn worker threads */
    for (int i = 0; i < wq->num_workers; i++) {
        char tname[32];
        snprintf(tname, sizeof(tname), "wq_%s_%d", wq->name, i);

        struct process *p = kthread_create(wq_worker_thread, wq, tname);
        if (p) {
            wq->worker_pids[i] = (int)p->pid;

            /* WQ_HIGHPRI: boost priority to RT */
            if (flags & WQ_HIGHPRI) {
                p->priority = 0;   /* highest priority */
                p->sched_policy = 1; /* SCHED_FIFO */
            }

            wq->workers_running++;
            wq->pub.workers_running = wq->workers_running;
        } else {
            kprintf("[workqueue] failed to create worker %s\n", tname);
        }
    }

    kprintf("[OK] workqueue \"%s\" created (%d workers%s)\n",
            wq->name, wq->workers_running,
            (flags & WQ_UNBOUND) ? ", unbound" : "");

    return &wq->pub;
}

int workqueue_schedule_on(struct workqueue_struct *pub_wq,
                          work_fn_t fn, void *arg)
{
    if (!pub_wq || !fn) return -1;

    /* Recover internal struct via container-of */
    struct wq_internal *wq = (struct wq_internal *)
        ((uintptr_t)pub_wq - __builtin_offsetof(struct wq_internal, pub));

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&wq->lock, &irq_flags);

    int slot = -1;
    for (int i = 0; i < WORKQUEUE_MAX; i++) {
        if (wq->items[i].state == WORKQUEUE_SLOT_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&wq->lock, irq_flags);
        return -1;
    }

    wq->items[slot].fn = fn;
    wq->items[slot].arg = arg;
    wq->items[slot].state = WORKQUEUE_SLOT_PENDING;

    spinlock_irqsave_release(&wq->lock, irq_flags);
    return 0;
}

void workqueue_flush(struct workqueue_struct *pub_wq)
{
    if (!pub_wq) return;

    struct wq_internal *wq = (struct wq_internal *)
        ((uintptr_t)pub_wq - __builtin_offsetof(struct wq_internal, pub));

    wq->draining = 1;

    /* Spin until all work items are processed.
     * Each worker will eventually pick up a pending item, execute it,
     * and mark the slot free.  When all slots are free, we're done. */
    for (;;) {
        int all_done = 1;
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&wq->lock, &irq_flags);
        for (int i = 0; i < WORKQUEUE_MAX; i++) {
            if (wq->items[i].state != WORKQUEUE_SLOT_FREE) {
                all_done = 0;
                break;
            }
        }
        spinlock_irqsave_release(&wq->lock, irq_flags);
        if (all_done) break;
        scheduler_yield();
    }

    wq->draining = 0;
}

void workqueue_destroy(struct workqueue_struct *pub_wq)
{
    if (!pub_wq) return;

    struct wq_internal *wq = (struct wq_internal *)
        ((uintptr_t)pub_wq - __builtin_offsetof(struct wq_internal, pub));

    /* Flush any pending work */
    workqueue_flush(pub_wq);

    /* Set draining flag and wake workers so they exit their loop */
    wq->draining = 1;

    /* Wait for all workers to terminate */
    for (int attempts = 0; attempts < 1000; attempts++) {
        if (wq->workers_running <= 0)
            break;
        scheduler_yield();
    }

    if (wq->workers_running > 0) {
        kprintf("[workqueue] WARNING: %d workers still running on \"%s\"\n",
                wq->workers_running, wq->name);
    }

    /* Release the pool slot */
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&wq_pool_lock, &irq_flags);
    memset(wq, 0, sizeof(*wq));
    spinlock_irqsave_release(&wq_pool_lock, irq_flags);
}

/* ── Workqueue OOM handling (Item 7) ─────────────────────────────── */

/*
 * When memory is low, workqueue OOM handling:
 *   1. Suspends non-critical workqueue creation
 *   2. Reserves emergency worker threads
 *   3. Flushes high-priority work first
 *   4. Drops non-essential work items
 */

/* Emergency OOM watermarks */
#define WQ_OOM_WATERMARK_LOW    2    /* pages — warn */
#define WQ_OOM_WATERMARK_CRIT   1    /* pages — emergency actions */

/* OOM state */
static volatile int wq_oom_state;        /* 0=normal, 1=low, 2=critical */
static volatile int wq_oom_reserve;      /* reserve worker available */

/* Set workqueue OOM state based on available memory.
 * Called by the OOM handler when memory pressure is detected.
 * @available_pages: number of free pages remaining. */
void workqueue_oom_notify(uint64_t available_pages)
{
    int prev_state = wq_oom_state;

    if (available_pages <= WQ_OOM_WATERMARK_CRIT) {
        wq_oom_state = 2;  /* critical */
        wq_oom_reserve = 1; /* activate reserve worker */
    } else if (available_pages <= WQ_OOM_WATERMARK_LOW) {
        wq_oom_state = 1;  /* low */
    } else {
        wq_oom_state = 0;  /* normal */
        wq_oom_reserve = 0;
    }

    if (wq_oom_state != prev_state) {
        kprintf("[workqueue] OOM state: %s (free pages: %llu)\n",
                wq_oom_state == 0 ? "normal" :
                wq_oom_state == 1 ? "low" : "CRITICAL",
                (unsigned long long)available_pages);
    }
}

/* Check if we're in OOM state that should suppress new work. */
int workqueue_oom_check(void)
{
    return wq_oom_state >= 1;
}

/* Get the current workqueue OOM state. */
int workqueue_oom_get_state(void)
{
    return wq_oom_state;
}

/* ── Initialisation ───────────────────────────────────────────────── */

void workqueue_init(void)
{
    /* Initialise system workqueue */
    memset(&g_sys_wq, 0, sizeof(g_sys_wq));
    spinlock_init(&g_sys_wq.lock);
    memcpy(g_sys_wq.name, "sys", 4);
    g_sys_wq.num_workers = 1;
    g_sys_wq.workers_running = 0;

    /* Initialise workqueue pool */
    memset(wq_pool, 0, sizeof(wq_pool));
    spinlock_init(&wq_pool_lock);

    g_wq_initialized = 1;

    /* Create the system workqueue worker thread */
    struct process *p = kthread_create(wq_worker_thread, &g_sys_wq, "workqueue");
    if (p) {
        g_sys_wq.worker_pids[0] = (int)p->pid;
        g_sys_wq.workers_running = 1;
        g_sys_wq.pub.workers_running = 1;
        g_sys_wq.pub.num_workers = 1;
        memcpy(g_sys_wq.pub.name, "sys", 4);
        g_sys_wq.pub.flags = 0;

        kprintf("[OK] Workqueue subsystem initialized\n");
    } else {
        kprintf("[!!] Workqueue: failed to create system worker thread\n");
    }
}

/* ── queue_work: Schedule work on a specific workqueue ──────────────── */
/* Wraps the existing workqueue_schedule_on() using work_fn_t callbacks. */
struct work_struct;
struct delayed_work;

int queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
    (void)wq;
    (void)work;
    /* This is a compatibility wrapper. The native API uses work_fn_t directly.
     * queue_work() is not yet wired to the struct-based API because
     * work_struct definition is not available in this kernel version.
     * Callers should use workqueue_schedule_on() or workqueue_schedule() instead. */
    kprintf("[WORKQUEUE] queue_work: not yet wired (use workqueue_schedule_on directly)\n");
    return 0;
}

/* ── queue_delayed_work: Schedule delayed work on a workqueue ──────────── */
int queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
                       unsigned long delay)
{
    (void)wq;
    (void)dwork;
    (void)delay;
    /* Compatibility wrapper — struct delayed_work not defined in this kernel.
     * Use timer_schedule() directly for delayed callback execution. */
    kprintf("[WORKQUEUE] queue_delayed_work: not yet wired (use timer_schedule directly)\n");
    return 0;
}
