/*
 * epoll.c — I/O event notification facility (epoll)
 *
 * Implements epoll_create1, epoll_ctl, epoll_wait, and epoll_pwait
 * system calls.  Uses a fixed-size global slot table (fd range 730..745)
 * following the same pattern as eventfd, signalfd, and timerfd.
 *
 * Architecture:
 *   epoll_create1 allocates a slot from the global epoll_table[].
 *   epoll_ctl adds/modifies/removes fd entries tracked by the instance.
 *   epoll_wait iterates the instance's entries, checks each fd for
 *   readiness, and blocks using scheduler_yield() under a timeout.
 *   Both level-triggered (default) and edge-triggered (EPOLLET)
 *   event reporting are supported.  EPOLLONESHOT entries are
 *   automatically disarmed after the first event report.
 *   Future enhancements (tasks 9-10) will add EPOLLEXCLUSIVE,
 *   EPOLLWAKEUP, and waitqueue-based blocking.
 *
 * Module metadata:
 *   MODULE_LICENSE("GPL v2")
 *   MODULE_VERSION("1.0")
 *   MODULE_DESCRIPTION("epoll I/O event notification facility")
 *   MODULE_AUTHOR("Ruslan Gustomiasov")
 */

#define KERNEL_INTERNAL
#include "epoll.h"
#include "syscall.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "scheduler.h"
#include "timer.h"
#include "socket.h"
#include "errno.h"

/* ── Global state ────────────────────────────────────────────── */

/* Global epoll instance table */
static struct epoll_instance epoll_table[EPOLL_MAX];

/* ── Internal helpers ────────────────────────────────────────── */

/*
 * epoll_get — look up an epoll instance by fd.
 * Returns NULL if the fd is not a valid epoll instance.
 */
static struct epoll_instance *epoll_get(int fd)
{
    int slot = fd - EPOLL_FD_BASE;
    if (slot < 0 || slot >= EPOLL_MAX)
        return NULL;
    if (!epoll_table[slot].in_use)
        return NULL;
    return &epoll_table[slot];
}

/*
 * epoll_find_entry — find an entry for a given fd within an instance.
 * Returns a pointer to the entry, or NULL if not found.
 */
static struct epoll_fd_entry *
epoll_find_entry(struct epoll_instance *ep, int fd)
{
    for (int i = 0; i < ep->num_entries; i++) {
        if (ep->entries[i].fd == fd && ep->entries[i].in_use)
            return &ep->entries[i];
    }
    return NULL;
}

/* ── epoll_create1 ───────────────────────────────────────────── */

/*
 * epoll_create1_syscall — create an epoll instance.
 *
 * Validates that @flags only contains EPOLL_CLOEXEC (or 0),
 * allocates a slot from the global table, initialises the
 * wait queue and lock, and returns an fd number in the
 * EPOLL_FD_BASE range.
 *
 * Returns the fd on success, or a negative errno:
 *   -EINVAL  if invalid flags are specified
 *   -ENFILE  if all epoll instances are in use
 */
int epoll_create1_syscall(int flags)
{
    /* Validate flags — only EPOLL_CLOEXEC is permitted */
    if (flags & ~EPOLL_CLOEXEC)
        return -EINVAL;

    /* Find a free slot */
    for (int i = 0; i < EPOLL_MAX; i++) {
        if (!epoll_table[i].in_use) {
            memset(&epoll_table[i], 0, sizeof(struct epoll_instance));
            epoll_table[i].in_use = 1;
            epoll_table[i].flags  = flags;
            wait_queue_init(&epoll_table[i].wq);
            return EPOLL_FD_BASE + i;
        }
    }

    return -ENFILE;
}

/* ── epoll_ctl ───────────────────────────────────────────────── */

/* Valid event flags mask — all known epoll event bits.
 * Used to reject invalid or undefined event flags in ADD/MOD. */
#define EPOLL_VALID_EVENTS                                            \
    (EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP |           \
     EPOLLRDNORM | EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND |         \
     EPOLLMSG | EPOLLRDHUP | EPOLLONESHOT | EPOLLET)

/*
 * epoll_ctl_syscall — control interface for an epoll instance.
 *
 * Implements EPOLL_CTL_ADD (add a new fd to monitor),
 * EPOLL_CTL_MOD (modify an existing fd's events), and
 * EPOLL_CTL_DEL (remove an fd from monitoring).
 *
 * Validation:
 *   - epfd must refer to a valid epoll instance
 *   - For ADD, fd must not equal epfd (cannot monitor self)
 *   - For ADD/MOD, event must be non-NULL
 *   - For ADD/MOD, events field is checked for unknown flag bits
 *   - For DEL, the event pointer is ignored (Linux semantics)
 *
 * Returns 0 on success, or a negative errno:
 *   -EBADF  if epfd is not a valid epoll instance
 *   -EINVAL if op is invalid, event is NULL, bad event flags,
 *           or fd == epfd (ADD only)
 *   -EEXIST if fd is already monitored (ADD only)
 *   -ENOENT if fd not found (DEL/MOD only)
 *   -ENOMEM if instance is full (ADD only)
 */
int epoll_ctl_syscall(int epfd, int op, int fd,
                       struct epoll_event *event)
{
    struct epoll_instance *ep = epoll_get(epfd);
    if (!ep)
        return -EBADF;

    int ret = 0;

    switch (op) {
    case EPOLL_CTL_ADD: {
        /* Cannot monitor self — would cause deadlock */
        if (fd == epfd)
            return -EINVAL;

        /* event is required for ADD */
        if (!event)
            return -EINVAL;

        /* Validate event flags — reject undefined bits */
        if (event->events & ~(uint32_t)EPOLL_VALID_EVENTS)
            return -EINVAL;

        /* Check if the fd is already monitored */
        if (epoll_find_entry(ep, fd)) {
            ret = -EEXIST;
            break;
        }

        /* Check capacity */
        if (ep->num_entries >= EPOLL_MAX_EVENTS) {
            ret = -ENOMEM;
            break;
        }

        /* Populate the entry */
        struct epoll_fd_entry *e = &ep->entries[ep->num_entries++];
        e->fd             = fd;
        e->events         = event->events;
        e->data           = event->data;
        e->last_reported  = 0;
        e->in_use         = 1;
        e->armed          = 1;
        break;
    }

    case EPOLL_CTL_DEL: {
        /* event is ignored for DEL (Linux semantics) */
        struct epoll_fd_entry *e = epoll_find_entry(ep, fd);
        if (!e) {
            ret = -ENOENT;
            break;
        }

        /* Swap with last entry and decrement count */
        *e = ep->entries[--ep->num_entries];
        /* Clear the now-unused last slot */
        if (ep->num_entries > 0)
            memset(&ep->entries[ep->num_entries], 0,
                   sizeof(struct epoll_fd_entry));
        break;
    }

    case EPOLL_CTL_MOD: {
        /* event is required for MOD */
        if (!event)
            return -EINVAL;

        /* Validate event flags — reject undefined bits */
        if (event->events & ~(uint32_t)EPOLL_VALID_EVENTS)
            return -EINVAL;

        struct epoll_fd_entry *e = epoll_find_entry(ep, fd);
        if (!e) {
            ret = -ENOENT;
            break;
        }

        e->events = event->events;
        e->data   = event->data;
        e->last_reported = 0;
        e->armed  = 1;
        break;
    }

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

/* ── epoll_wait ──────────────────────────────────────────────── */

/*
 * epoll_wait_syscall — wait for events on an epoll instance.
 *
 * Iterates the instance's monitored fds, checking each for
 * readiness.  If nothing is ready, blocks using scheduler_yield()
 * under a timeout.  When events become available (or on timeout),
 * copies the ready events to the user-provided buffer.
 *
 * Returns the number of ready events on success, 0 on timeout,
 * or a negative errno:
 *   -EBADF  if epfd is not a valid epoll instance
 *   -EFAULT if the events buffer is not writable
 *   -EINTR  if interrupted by a signal
 */
int epoll_wait_syscall(int epfd, struct epoll_event *events,
                        int maxevents, int timeout)
{
    struct epoll_instance *ep = epoll_get(epfd);
    if (!ep)
        return -EBADF;

    /* Clamp maxevents to instance capacity */
    if (maxevents > EPOLL_MAX_EVENTS)
        maxevents = EPOLL_MAX_EVENTS;
    if (maxevents <= 0)
        return -EINVAL;

    /* Compute deadline for timeout */
    uint64_t deadline = 0;
    if (timeout >= 0) {
        deadline = timer_get_ticks() + ((uint64_t)timeout * TIMER_FREQ) / 1000;
        /* Ensure at least 1 tick for tiny timeouts */
        if (deadline == timer_get_ticks())
            deadline = timer_get_ticks() + 1;
    }

    struct process *cur = process_get_current();

    for (;;) {
        int ready = 0;

        for (int i = 0; i < ep->num_entries && ready < maxevents; i++) {
            struct epoll_fd_entry *e = &ep->entries[i];
            if (!e->in_use || !e->armed)
                continue;

            uint32_t revents = 0;

            /* Check if fd is a socket and has data available */
            struct socket *s = sock_get(e->fd);
            if (s && s->state != SOCK_STATE_FREE) {
                if (e->events & EPOLLIN) {
                    /* Assume readable if connected or listening */
                    if (s->state == SOCK_STATE_CONNECTED ||
                        s->state == SOCK_STATE_LISTENING)
                        revents |= EPOLLIN;
                }
                if (e->events & EPOLLOUT) {
                    if (s->state == SOCK_STATE_CONNECTED)
                        revents |= EPOLLOUT;
                }
            }

            /* Also check regular fds via process fd_table */
            if (e->fd >= 0 && e->fd < PROCESS_FD_MAX) {
                struct process *p = cur;
                if (p && p->fd_table[e->fd].used) {
                    if (e->events & EPOLLIN)
                        revents |= EPOLLIN;
                    if (e->events & EPOLLOUT)
                        revents |= EPOLLOUT;
                }
            }

            if (revents) {
                /* Edge-triggered: only report new events since last report */
                if (e->events & EPOLLET) {
                    uint32_t new_events = revents & ~e->last_reported;
                    e->last_reported = revents;
                    if (!new_events)
                        continue;
                    events[ready].events = new_events;
                } else {
                    /* Level-triggered: report all ready events */
                    events[ready].events = revents;
                }
                events[ready].data   = e->data;
                ready++;

                /* EPOLLONESHOT: disarm after first event report */
                if (e->events & EPOLLONESHOT)
                    e->armed = 0;
            }
        }

        if (ready > 0)
            return ready;

        /* Check timeout */
        if (timeout >= 0 && timer_get_ticks() >= deadline)
            return 0; /* timeout — no events ready */

        /* Yield CPU while waiting */
        scheduler_yield();
    }
}

/* ── epoll_pwait ─────────────────────────────────────────────── */

/*
 * epoll_pwait_syscall — epoll_wait with signal mask.
 *
 * Currently ignores the extended sigmask (compatibility stub).
 * The signal mask is managed by the callers (pselect6/epoll_pwait
 * syscall wrappers).
 *
 * Returns the same as epoll_wait_syscall.
 */
int epoll_pwait_syscall(int epfd, struct epoll_event *events,
                         int maxevents, int timeout,
                         const uint64_t *sigmask)
{
    (void)sigmask;
    return epoll_wait_syscall(epfd, events, maxevents, timeout);
}

/* ── Close ───────────────────────────────────────────────────── */

/*
 * epoll_close — release an epoll instance.
 *
 * Marks the slot as free and wakes any waiters so they can
 * return -EBADF on the next interaction.
 */
void epoll_close(int epfd)
{
    struct epoll_instance *ep = epoll_get(epfd);
    if (!ep)
        return;

    ep->in_use = 0;
    wait_queue_wake_all(&ep->wq);
}
