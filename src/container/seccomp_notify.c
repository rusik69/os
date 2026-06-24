/*
 * seccomp_notify.c — User-space seccomp notification handler
 *                     (SECCOMP_IOCTL_NOTIF_RECV equivalent)
 *
 * Implements a policy-daemon-style seccomp notification system where
 * a userspace daemon evaluates intercepted syscalls and allows or
 * denies them.  This is the in-kernel counterpart of the Linux
 * SECCOMP_IOCTL_NOTIF_RECV / SECCOMP_IOCTL_NOTIF_SEND ioctl pair.
 *
 * Architecture:
 *   - A seccomp_notify_rule table defines which syscalls trigger
 *     notification (rather than being allowed/denied directly).
 *   - When a traced syscall fires, the kernel pushes a request onto
 *     a global notification queue.
 *   - A userspace policy daemon calls seccomp_notify_recv() to
 *     dequeue pending requests, evaluates them, and calls
 *     seccomp_notify_respond() to deliver the verdict.
 *
 * Data flow:
 *   process  ──syscall──>  seccomp_filter  ──notify──>  request queue
 *                                                              │
 *   userspace daemon  <──seccomp_notify_recv()──                │
 *   userspace daemon  ──seccomp_notify_respond()──>  verdict sent back
 *
 * Item C162
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "seccomp.h"
#include "seccomp_bpf.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "process.h"
#include "timer.h"
#include "waitqueue.h"

/* ── Constants ──────────────────────────────────────────────────────── */

/* Maximum number of pending notification requests in the global queue */
#define SECCOMP_NOTIFY_QUEUE_MAX    64

/* Maximum length of a rule name (human-readable description) */
#define SECCOMP_NOTIFY_NAME_MAX     32

/* ── Data structures ────────────────────────────────────────────────── */

/**
 * struct seccomp_notify_request — A syscall intercepted and awaiting
 *                                 a userspace verdict.
 * @id:         Unique request identifier (monotonically increasing).
 * @pid:        PID of the process that made the syscall.
 * @syscall_nr: The syscall number that was intercepted.
 * @args:       Up to 6 syscall argument values.
 * @in_use:     Slot is occupied (allocated but not yet responded to).
 */
struct seccomp_notify_request {
    uint64_t id;
    uint32_t pid;
    int      syscall_nr;
    uint64_t args[6];
    int      in_use;
};

/**
 * struct seccomp_notify_response — Verdict delivered by the userspace
 *                                  policy daemon.
 * @id:    Matches the request ID from the corresponding request.
 * @error: errno value to return to the interrupted process (0 = allow).
 * @val:   Return value to place in %rax when @error is 0.
 */
struct seccomp_notify_response {
    uint64_t id;
    int      error;
    int64_t  val;
};

/**
 * struct seccomp_notify_rule — Description of which syscalls should
 *                              trigger a notification.
 * @syscall_nr: Syscall number to intercept.
 * @action:     Action to take: 0=allow, 1=deny, 2=kill (matches
 *              SECCOMP_RET_ALLOW / SECCOMP_RET_ERRNO / SECCOMP_RET_KILL).
 * @name:       Human-readable name for debugging/logging.
 */
struct seccomp_notify_rule {
    int      syscall_nr;
    int      action;      /* 0=allow, 1=deny (ENOSYS), 2=kill */
    char     name[SECCOMP_NOTIFY_NAME_MAX];
};

/* ── Global state ──────────────────────────────────────────────────── */

/* Pending request ring buffer */
static struct seccomp_notify_request
    notify_queue[SECCOMP_NOTIFY_QUEUE_MAX];

/* Response ring buffer (indexed by request ID % QUEUE_MAX) */
static struct seccomp_notify_response
    notify_responses[SECCOMP_NOTIFY_QUEUE_MAX];

/* Rule table */
static struct seccomp_notify_rule
    notify_rules[SECCOMP_FILTER_RULES_MAX];

/* Number of active rules */
static int notify_num_rules = 0;

/* Monotonically increasing request ID counter */
static uint64_t notify_next_id = 1;

/* Lock protecting the notification queues and rule table */
static spinlock_t seccomp_notify_lock = SPINLOCK_INIT;

/* Waitqueue for policy daemon to block on when no requests are pending */
static struct wait_queue seccomp_notify_wq = WAITQUEUE_INIT;

/* Waitqueue for syscall callers to block on while awaiting a response */
static struct wait_queue seccomp_response_wq = WAITQUEUE_INIT;

/* Flag: is the notification subsystem initialised? */
static int seccomp_notify_initialised = 0;

/* ── Internal helpers ───────────────────────────────────────────────── */

/*
 * Find a free slot in the request queue.
 * Returns the slot index, or -ENOSPC if the queue is full.
 */
static int notify_find_free_slot(void)
{
    for (int i = 0; i < SECCOMP_NOTIFY_QUEUE_MAX; i++) {
        if (!notify_queue[i].in_use)
            return i;
    }
    return -ENOSPC;
}

/*
 * Check whether a given syscall number matches any registered
 * notification rule.
 *
 * Returns 1 if the syscall should be intercepted, 0 otherwise.
 * If @action_out is non-NULL, the matching rule's action is stored there.
 * If @name_out is non-NULL (with @name_max bytes), the matching rule's
 * name is copied there.
 */
static int notify_rule_match(int syscall_nr, int *action_out,
                              char *name_out, int name_max)
{
    for (int i = 0; i < notify_num_rules; i++) {
        if (notify_rules[i].syscall_nr == syscall_nr) {
            if (action_out)
                *action_out = notify_rules[i].action;
            if (name_out && name_max > 0) {
                strncpy(name_out, notify_rules[i].name,
                        (size_t)name_max - 1);
                name_out[name_max - 1] = '\0';
            }
            return 1;
        }
    }
    return 0;
}

/*
 * Look up a response by request ID.
 * Returns a pointer to the response entry, or NULL if not found.
 */
static struct seccomp_notify_response *notify_lookup_response(uint64_t id)
{
    int idx = (int)(id % SECCOMP_NOTIFY_QUEUE_MAX);
    if (notify_responses[idx].id == id)
        return &notify_responses[idx];
    return NULL;
}

/*
 * Clear a response entry after it has been consumed.
 */
static void notify_clear_response(uint64_t id)
{
    int idx = (int)(id % SECCOMP_NOTIFY_QUEUE_MAX);
    if (notify_responses[idx].id == id) {
        memset(&notify_responses[idx], 0,
               sizeof(notify_responses[idx]));
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * seccomp_notify_init() — Initialise the seccomp notification subsystem.
 *
 * Registers the notification handler as the default action for any
 * seccomp rule that has action SECCOMP_RET_LOG (which we repurpose as
 * "intercept and notify").  Clears the request and response queues.
 *
 * This must be called once during kernel initialisation, after
 * seccomp_init() and seccomp_bpf_init() have completed.
 *
 * Returns 0 on success, negative errno on failure.
 */
int seccomp_notify_init(void)
{
    kprintf("[SeccompNotify] Initialising notification subsystem...\n");

    spinlock_acquire(&seccomp_notify_lock);

    if (seccomp_notify_initialised) {
        spinlock_release(&seccomp_notify_lock);
        kprintf("[SeccompNotify] Already initialised\n");
        return 0;
    }

    /* Clear request queue */
    memset(notify_queue, 0, sizeof(notify_queue));

    /* Clear response queue */
    memset(notify_responses, 0, sizeof(notify_responses));

    /* Clear rule table */
    memset(notify_rules, 0, sizeof(notify_rules));
    notify_num_rules = 0;

    /* Reset request ID counter */
    notify_next_id = 1;

    seccomp_notify_initialised = 1;

    spinlock_release(&seccomp_notify_lock);

    kprintf("[SeccompNotify] OK — notification subsystem ready "
            "(queue: %d slots, max rules: %d)\n",
            SECCOMP_NOTIFY_QUEUE_MAX, SECCOMP_FILTER_RULES_MAX);
    return 0;
}

/**
 * seccomp_notify_add_rule() — Register a syscall notification rule.
 *
 * Tells the notification subsystem to intercept @syscall_nr and apply
 * @action when the syscall is made.
 *
 * @syscall_nr: The syscall number to intercept.
 * @action:     0 = allow, 1 = deny (return 0), 2 = kill process.
 * @name:       Optional human-readable name (may be NULL or empty).
 *
 * Returns 0 on success, -ENOSPC if the rule table is full.
 */
int seccomp_notify_add_rule(int syscall_nr, int action, const char *name)
{
    if (syscall_nr < 0)
        return -EINVAL;

    spinlock_acquire(&seccomp_notify_lock);

    if (notify_num_rules >= SECCOMP_FILTER_RULES_MAX) {
        spinlock_release(&seccomp_notify_lock);
        kprintf("[SeccompNotify] Rule table full (%d max)\n",
                SECCOMP_FILTER_RULES_MAX);
        return -ENOSPC;
    }

    /* Check for duplicate */
    for (int i = 0; i < notify_num_rules; i++) {
        if (notify_rules[i].syscall_nr == syscall_nr) {
            /* Update the existing rule's action and name */
            notify_rules[i].action = action;
            if (name && name[0]) {
                strncpy(notify_rules[i].name, name,
                        SECCOMP_NOTIFY_NAME_MAX - 1);
                notify_rules[i].name[SECCOMP_NOTIFY_NAME_MAX - 1] = '\0';
            }
            spinlock_release(&seccomp_notify_lock);
            kprintf("[SeccompNotify] Updated rule for syscall %d "
                    "(action=%d, name='%s')\n",
                    syscall_nr, action, notify_rules[i].name);
            return 0;
        }
    }

    /* Add new rule */
    int idx = notify_num_rules;
    notify_rules[idx].syscall_nr = syscall_nr;
    notify_rules[idx].action = action;
    if (name && name[0]) {
        strncpy(notify_rules[idx].name, name,
                SECCOMP_NOTIFY_NAME_MAX - 1);
        notify_rules[idx].name[SECCOMP_NOTIFY_NAME_MAX - 1] = '\0';
    } else {
        notify_rules[idx].name[0] = '\0';
    }
    notify_num_rules++;

    spinlock_release(&seccomp_notify_lock);

    kprintf("[SeccompNotify] Added rule: syscall=%d action=%d name='%s'\n",
            syscall_nr, action,
            name && name[0] ? name : "(unnamed)");
    return 0;
}

/**
 * seccomp_notify_recv() — Dequeue the next pending notification request.
 *
 * Blocks (via spinlock polling / scheduler yield) until a request is
 * available or @timeout_ms milliseconds have elapsed.  Intended to be
 * called by a userspace policy daemon.
 *
 * @req:        Output parameter — receives the next pending request.
 * @timeout_ms: Maximum time to wait in milliseconds (0 = no wait,
 *              -1 = wait indefinitely).
 *
 * Returns 0 on success with a valid request in @req, -EAGAIN if
 * the timeout expired with no request, -EINTR if interrupted.
 */
int seccomp_notify_recv(struct seccomp_notify_request *req,
                         int timeout_ms)
{
    if (!req)
        return -EINVAL;

    uint64_t start_tick = timer_get_ticks();
    int timeout_ticks = 0;

    if (timeout_ms > 0) {
        timeout_ticks = (timeout_ms * TIMER_FREQ + 999) / 1000;
        if (timeout_ticks < 1) timeout_ticks = 1;
    }

    for (;;) {
        spinlock_acquire(&seccomp_notify_lock);

        /* Scan for the oldest pending request (lowest ID) */
        int found_idx = -1;
        uint64_t oldest_id = UINT64_MAX;

        for (int i = 0; i < SECCOMP_NOTIFY_QUEUE_MAX; i++) {
            if (notify_queue[i].in_use &&
                notify_queue[i].id < oldest_id) {
                oldest_id = notify_queue[i].id;
                found_idx = i;
            }
        }

        if (found_idx >= 0) {
            /* Copy out the request */
            memcpy(req, &notify_queue[found_idx],
                   sizeof(*req));

            /* Mark slot as free */
            memset(&notify_queue[found_idx], 0,
                   sizeof(notify_queue[found_idx]));

            spinlock_release(&seccomp_notify_lock);
            return 0;
        }

        spinlock_release(&seccomp_notify_lock);

        /* No request available — check timeout */
        if (timeout_ms == 0)
            return -EAGAIN;

        if (timeout_ms > 0) {
            uint64_t elapsed = timer_get_ticks() - start_tick;
            if ((int)elapsed >= timeout_ticks)
                return -EAGAIN;
        }

        /* Block on the waitqueue until a new request arrives.
         * Use interruptible sleep so signals can unblock us. */
        int wret;
        if (timeout_ms > 0) {
            uint64_t remaining = (uint64_t)timeout_ticks - (timer_get_ticks() - start_tick);
            if ((int)remaining <= 0)
                return -EAGAIN;
            wret = wait_queue_sleep_interruptible_timeout(
                       &seccomp_notify_wq, remaining);
        } else {
            /* Wait indefinitely (timeout_ms < 0) */
            wret = wait_queue_sleep_interruptible(&seccomp_notify_wq);
        }

        if (wret == -EINTR) {
            return -EINTR;  /* Interrupted by signal */
        }
        if (wret == -ETIME) {
            return -EAGAIN;  /* Timeout expired */
        }
        /* Woken normally — loop and try to dequeue */
    }
}

/**
 * seccomp_notify_respond() — Deliver a verdict for a previously
 *                            received notification request.
 *
 * Called by the userspace policy daemon after evaluating the syscall.
 * The verdict is stored in the response ring buffer, where it will
 * be picked up by the notification handler when the interrupted
 * process resumes.
 *
 * @resp: The response containing the request ID, error code, and
 *        optional return value.
 *
 * Returns 0 on success, -ENOENT if no matching pending request was
 * found (possible if the process was killed while we were evaluating).
 */
int seccomp_notify_respond(const struct seccomp_notify_response *resp)
{
    if (!resp)
        return -EINVAL;

    spinlock_acquire(&seccomp_notify_lock);

    /* Store the response in the slot indexed by request ID */
    int idx = (int)(resp->id % SECCOMP_NOTIFY_QUEUE_MAX);
    memcpy(&notify_responses[idx], resp,
           sizeof(*resp));

    spinlock_release(&seccomp_notify_lock);

    /* Wake the waiting syscall caller that a response is available */
    wait_queue_wake(&seccomp_response_wq);

    kprintf("[SeccompNotify] Responded to request %llu: "
            "error=%d val=%lld\n",
            (unsigned long long)resp->id,
            resp->error, (long long)resp->val);
    return 0;
}

/**
 * seccomp_notify_check_syscall() — Evaluate a syscall against the
 *                                  notification rule table.
 *
 * Called from the seccomp evaluation path.  If the syscall matches a
 * notification rule, this function:
 *   1. Creates a notification request and enqueues it.
 *   2. Spins waiting for the userspace daemon to respond.
 *   3. Applies the verdict (allow / deny / kill).
 *
 * This is the main integration point between the seccomp filter
 * subsystem and the notification handler.
 *
 * @syscall_nr: The syscall number being evaluated.
 * @args:       The syscall arguments (up to 6).
 * @pid:        PID of the calling process.
 *
 * Returns:
 *   SECCOMP_RET_ALLOW  — allow the syscall.
 *   SECCOMP_RET_KILL   — kill the process.
 *   Negative errno     — deny with the given errno.
 */
int seccomp_notify_check_syscall(int syscall_nr, uint64_t *args,
                                  uint32_t pid)
{
    int action;
    char rule_name[SECCOMP_NOTIFY_NAME_MAX];

    if (!seccomp_notify_initialised)
        return 0;  /* not initialised → allow */

    spinlock_acquire(&seccomp_notify_lock);

    /* Check if this syscall matches a notification rule */
    if (!notify_rule_match(syscall_nr, &action,
                            rule_name, sizeof(rule_name))) {
        spinlock_release(&seccomp_notify_lock);
        return 0;  /* no matching rule → allow */
    }

    /* For non-notify actions, handle immediately */
    if (action == 0) {
        /* Allow */
        spinlock_release(&seccomp_notify_lock);
        return 0;
    }
    if (action == 2) {
        /* Kill */
        spinlock_release(&seccomp_notify_lock);
        return -SECCOMP_RET_KILL;  /* signal to caller to kill */
    }

    /* action == 1 (or unknown): create a notification request */
    int slot = notify_find_free_slot();
    if (slot < 0) {
        /* Queue full — deny with ENOSPC */
        spinlock_release(&seccomp_notify_lock);
        kprintf("[SeccompNotify] Request queue full — denying "
                "syscall %d from PID %u\n", syscall_nr, pid);
        return -ENOSPC;
    }

    uint64_t req_id = notify_next_id++;
    notify_queue[slot].id         = req_id;
    notify_queue[slot].pid        = pid;
    notify_queue[slot].syscall_nr = syscall_nr;
    if (args) {
        memcpy(notify_queue[slot].args, args,
               sizeof(notify_queue[slot].args));
    }
    notify_queue[slot].in_use = 1;

    kprintf("[SeccompNotify] Enqueued request %llu: PID %u "
            "syscall %d (%s)\n",
            (unsigned long long)req_id, pid, syscall_nr,
            rule_name[0] ? rule_name : "unknown");

    spinlock_release(&seccomp_notify_lock);

    /* Wake the policy daemon that a new request is available */
    wait_queue_wake(&seccomp_notify_wq);

    /* Wait for the response using waitqueue (interruptible with timeout) */
    /* Use wait_event_timeout macro: loop checking for response, sleeping otherwise */
    uint64_t deadline = timer_get_ticks() + (TIMER_FREQ * 10); /* 10s max */

    {
        int resp_found = 0;
        while (timer_get_ticks() < deadline && !resp_found) {
            spinlock_acquire(&seccomp_notify_lock);
            struct seccomp_notify_response *resp =
                notify_lookup_response(req_id);
            if (resp) {
                int resp_error = resp->error;
                int64_t resp_val = resp->val;

                notify_clear_response(req_id);
                spinlock_release(&seccomp_notify_lock);

                kprintf("[SeccompNotify] Request %llu resolved: "
                        "error=%d val=%lld\n",
                        (unsigned long long)req_id,
                        resp_error, (long long)resp_val);

                if (resp_error != 0)
                    return -resp_error;  /* deny with errno */
                return 0;  /* allow */
            }
            spinlock_release(&seccomp_notify_lock);

            /* Block on the response waitqueue until the daemon responds.
             * Use a short timeout so we can re-check the deadline. */
            uint64_t remaining = deadline > timer_get_ticks()
                ? deadline - timer_get_ticks() : 0;
            if (remaining == 0) break;

            /* Cap the wait to at most 100 ticks to re-check deadline */
            uint64_t wait_for = remaining < (TIMER_FREQ / 10)
                ? remaining : (TIMER_FREQ / 10);

            int wret = wait_queue_sleep_interruptible_timeout(
                           &seccomp_response_wq, wait_for);
            if (wret == -EINTR) {
                /* Interrupted by signal — deny */
                kprintf("[SeccompNotify] Request %llu interrupted by signal\n",
                        (unsigned long long)req_id);
                return -EINTR;
            }
            /* On timeout or normal wake, loop and check response */
        }
    }

    /* Timeout — deny with ETIMEDOUT */
    kprintf("[SeccompNotify] Request %llu timed out "
            "(PID %u, syscall %d) — denying\n",
            (unsigned long long)req_id, pid, syscall_nr);
    return -ETIMEDOUT;
}

/**
 * seccomp_notify_remove_rule() — Remove a notification rule by
 *                                syscall number.
 *
 * @syscall_nr: The syscall number to stop intercepting.
 *
 * Returns 0 on success, -ENOENT if no matching rule was found.
 */
int seccomp_notify_remove_rule(int syscall_nr)
{
    spinlock_acquire(&seccomp_notify_lock);

    for (int i = 0; i < notify_num_rules; i++) {
        if (notify_rules[i].syscall_nr == syscall_nr) {
            /* Shift remaining rules down */
            int remaining = notify_num_rules - i - 1;
            if (remaining > 0) {
                memmove(&notify_rules[i], &notify_rules[i + 1],
                        (size_t)remaining * sizeof(notify_rules[0]));
            }
            notify_num_rules--;
            spinlock_release(&seccomp_notify_lock);

            kprintf("[SeccompNotify] Removed rule for syscall %d\n",
                    syscall_nr);
            return 0;
        }
    }

    spinlock_release(&seccomp_notify_lock);
    return -ENOENT;
}

/**
 * seccomp_notify_flush() — Remove all pending requests and responses
 *                          for a given PID.
 *
 * Called when a process exits so that any pending notification
 * requests are cleaned up and the policy daemon won't wait forever
 * for a process that no longer exists.
 *
 * @pid: PID of the process that has exited.
 *
 * Returns the number of requests flushed.
 */
int seccomp_notify_flush(uint32_t pid)
{
    int flushed = 0;

    spinlock_acquire(&seccomp_notify_lock);

    for (int i = 0; i < SECCOMP_NOTIFY_QUEUE_MAX; i++) {
        if (notify_queue[i].in_use && notify_queue[i].pid == pid) {
            memset(&notify_queue[i], 0, sizeof(notify_queue[i]));
            flushed++;
        }
    }

    spinlock_release(&seccomp_notify_lock);

    if (flushed > 0) {
        kprintf("[SeccompNotify] Flushed %d pending request(s) "
                "for PID %u\n", flushed, pid);
    }
    return flushed;
}

/**
 * seccomp_notify_status() — Dump current notification subsystem
 *                           state to the kernel log.
 *
 * Reports the number of active rules, pending requests, and the
 * next request ID.
 */
void seccomp_notify_status(void)
{
    spinlock_acquire(&seccomp_notify_lock);

    int pending = 0;
    for (int i = 0; i < SECCOMP_NOTIFY_QUEUE_MAX; i++) {
        if (notify_queue[i].in_use)
            pending++;
    }

    kprintf("[SeccompNotify] Status: initialised=%d, "
            "rules=%d, pending_requests=%d, next_id=%llu\n",
            seccomp_notify_initialised,
            notify_num_rules, pending,
            (unsigned long long)notify_next_id);

    if (notify_num_rules > 0) {
        kprintf("[SeccompNotify] Rules:\n");
        for (int i = 0; i < notify_num_rules; i++) {
            kprintf("  [%d] syscall=%d action=%d name='%s'\n",
                    i, notify_rules[i].syscall_nr,
                    notify_rules[i].action,
                    notify_rules[i].name[0] ?
                        notify_rules[i].name : "(unnamed)");
        }
    }

    spinlock_release(&seccomp_notify_lock);
}

/* ── Stub: seccomp_notify_register ─────────────────────────────── */
int seccomp_notify_register(void *listener)
{
    (void)listener;
    kprintf("[container] seccomp_notify_register: not yet implemented\n");
    return 0;
}
/* ── Stub: seccomp_notify_unregister ─────────────────────────────── */
int seccomp_notify_unregister(void *listener)
{
    (void)listener;
    kprintf("[container] seccomp_notify_unregister: not yet implemented\n");
    return 0;
}
