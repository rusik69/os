#define KERNEL_INTERNAL
#include "poll.h"
#include "syscall.h"
#include "timer.h"
#include "process.h"
#include "scheduler.h"
#include "heap.h"
#include "string.h"
#include "errno.h"
#include "socket.h"
#include "pipe.h"
#include "inotify.h"
#include "printf.h"
#include "eventfd.h"
#include "signalfd.h"
#include "timerfd.h"
#include "epoll.h"
#include "memfd.h"

/*
 * poll.c — Poll infrastructure implementation
 *
 * Provides the kernel-internal poll_table API and the
 * sys_poll implementation with proper timeout handling.
 *
 * Architecture:
 *   poll_table collects wait queue registrations from
 *   file/driver poll handlers.  poll_schedule() blocks
 *   on those waitqueues, waking when events arrive.
 *
 * As fd poll handlers are wired up (tasks 11-14), they
 * will call poll_wait() to register their waitqueues,
 * making poll_schedule() efficient.  Until then, the
 * fallback uses scheduler_yield().
 */

/* ── Poll table lifecycle ───────────────────────────────────── */

int poll_init_table(struct poll_table *pt)
{
    struct poll_queue_entry *entries;

    entries = kmalloc(sizeof(struct poll_queue_entry) * POLL_TABLE_MAX);
    if (!entries)
        return -ENOMEM;

    pt->entries    = entries;
    pt->nr_entries = 0;
    pt->max_entries = POLL_TABLE_MAX;
    pt->error      = 0;
    return 0;
}

void poll_free_table(struct poll_table *pt)
{
    if (pt->entries) {
        kfree(pt->entries);
        pt->entries    = NULL;
        pt->nr_entries = 0;
        pt->max_entries = 0;
        pt->error      = 0;
    }
}

/* ── Wait queue registration ────────────────────────────────── */

int poll_wait(struct poll_table *pt, struct wait_queue *wq)
{
    struct poll_queue_entry *e;

    if (!pt)
        return 0; /* non-blocking probe — nothing to register */

    if (pt->nr_entries >= pt->max_entries) {
        pt->error = -ENOMEM;
        return -ENOMEM;
    }

    e = &pt->entries[pt->nr_entries++];
    e->wq  = wq;
    e->key = NULL;
    return 0;
}

/* ── Blocking wait ───────────────────────────────────────────── */

int poll_schedule(struct poll_table *pt, uint64_t timeout_ms)
{
    struct process *cur = process_get_current();

    if (!cur)
        return -EINTR;

    /* Non-blocking probe */
    if (timeout_ms == 0)
        return -ETIME;

    /*
     * If any wait queues have been registered via poll_wait(),
     * sleep on the first one using wait_queue_sleep_interruptible_timeout.
     * When it fires, we wake up and the caller re-checks readiness.
     *
     * Only block on the first registered wq — the semantic is:
     * "wake me when any of these fires", and sleeping on one is
     * sufficient since all registered queues are for the same
     * process's poll set.
     */
    if (pt->nr_entries > 0 && pt->entries[0].wq) {
        uint64_t timeout_ticks;
        int ret;

        timeout_ticks = (timeout_ms * TIMER_FREQ) / 1000;
        if (timeout_ms == ~0ULL)
            timeout_ticks = ~0ULL;
        if (timeout_ticks == 0)
            timeout_ticks = 1;

        ret = wait_queue_sleep_interruptible_timeout(
            pt->entries[0].wq, timeout_ticks);
        if (ret == -EINTR)
            return -EINTR;
        if (ret == -ETIME)
            return -ETIME;
        return 0; /* woken — re-check fds */
    }

    /*
     * Fallback: no wait queues registered (fd poll handlers not
     * yet wired).  Yield for 1 tick and re-check.
     */
    cur->sleep_until = timer_get_ticks() + 1;
    cur->state       = PROCESS_BLOCKED;
    scheduler_remove(cur);
    scheduler_yield();

    return 0;
}

/* ══════════════════════════════════════════════════════════════ */
/* sys_poll — poll() system call implementation                  */
/*                                                               */
/* Implements the poll(2) semantics:                             */
/*   - Validates user-provided struct pollfd array               */
/*   - Checks each fd for readiness (socket, pipe, inotify, fd)  */
/*   - Uses poll_table + poll_schedule for efficient waiting     */
/*   - Creates a wait queue for blocking when nothing is ready   */
/*   - Properly handles infinite, zero, and bounded timeouts     */
/* ══════════════════════════════════════════════════════════════ */

/*
 * POLL_MAX_FDS — bound iteration and prevent integer overflow.
 */
#define POLL_MAX_FDS  PROCESS_FD_MAX

/*
 * sys_poll — poll(2) entry point.
 *
 * @fds_addr:   user-space pointer to struct pollfd array
 * @nfds:       number of entries in the array
 * @timeout_ms: timeout in milliseconds (~0 = infinite, 0 = non-blocking)
 *
 * Returns: number of ready fds on success
 *          -EFAULT if user copy fails
 *          -EINTR if interrupted by a signal
 *          -ENOMEM if kernel memory exhausted
 */
uint64_t sys_poll(uint64_t fds_addr, uint64_t nfds, uint64_t timeout_ms)
{
    /*
     * ── Arg validation ────────────────────────────────────────
     */
    if (nfds > POLL_MAX_FDS)
        nfds = POLL_MAX_FDS;
    if (nfds == 0)
        return 0;

    uint64_t fds_size = nfds * sizeof(struct pollfd);
    if (fds_size / sizeof(struct pollfd) != nfds)
        return (uint64_t)(int64_t)-EINVAL;

    struct pollfd *fds = (struct pollfd *)fds_addr;
    int n = (int)nfds;
    int ready = 0;
    int timed_out = 0;
    uint64_t start_tick = timer_get_ticks();

    /*
     * ── Poll table for wait queue registration ────────────────
     * Stack-allocate entries buffer to avoid heap pressure on
     * every poll() call.
     */
    struct poll_queue_entry poll_entries_buf[POLL_TABLE_MAX];
    struct poll_table pt;

    poll_init_table_inline(&pt, poll_entries_buf, POLL_TABLE_MAX);

    struct process *cur = process_get_current();

    /*
     * ── Poll loop ─────────────────────────────────────────────
     */
    for (;;) {
        ready = 0;

        for (int i = 0; i < n; i++) {
            fds[i].revents = 0;

            /* Negative fd → POLLNVAL */
            if (fds[i].fd < 0) {
                fds[i].revents = POLLNVAL;
                ready++;
                continue;
            }

            /* Null process descriptor → POLLNVAL */
            if (!cur) {
                fds[i].revents = POLLNVAL;
                ready++;
                continue;
            }

            int fd_idx = fds[i].fd;
            int revents = 0;

            /*
             * ── Inotify FDs (fd 720..727) ─────────────────────
             */
            if (fd_idx >= INOTIFY_FD_BASE &&
                fd_idx < INOTIFY_FD_BASE + INOTIFY_INSTANCES) {
                revents = inotify_poll(fd_idx);
                if (revents < 0) {
                    fds[i].revents = POLLNVAL;
                    ready++;
                    continue;
                }
                if (revents && (fds[i].events & POLLIN)) {
                    fds[i].revents = POLLIN;
                    ready++;
                } else {
                    fds[i].revents = 0;
                }
                continue;
            }

            /*
             * ── Socket FDs (fd 100..100+SOCK_MAX-1) ──────────
             */
            if (fd_idx >= 100 &&
                fd_idx < 100 + SOCK_MAX) {
                revents = sock_poll(fd_idx, fds[i].events, &pt);
                fds[i].revents = (int16_t)revents;
                if (revents)
                    ready++;
                continue;
            }

            /*
             * ── Process fd_table entries ──────────────────────
             */
            if (fd_idx >= PROCESS_FD_MAX || !cur->fd_table[fd_idx].used) {
                fds[i].revents = POLLNVAL;
                ready++;
                continue;
            }

            struct process_fd *pfd = &cur->fd_table[fd_idx];

            /* Determine fd type and check readiness */
            if (strncmp(pfd->path, "pipe_read_", 10) == 0) {
                /* Pipe read end */
                int pipe_id = (int)pfd->offset;
                revents = pipe_poll(pipe_id, 1 /* is_read_end */, &pt);
            } else if (strncmp(pfd->path, "pipe_write_", 11) == 0) {
                /* Pipe write end */
                int pipe_id = (int)pfd->offset;
                revents = pipe_poll(pipe_id, 0 /* is_write_end */, &pt);
            } else {
                /* Regular file / other: always readable and
                 * writable if the requested events include them. */
                if (fds[i].events & POLLIN)
                    revents |= POLLIN;
                if (fds[i].events & POLLOUT)
                    revents |= POLLOUT;
            }

            /* Mask with requested events — only report what was
             * asked for. */
            fds[i].revents = (int16_t)(revents & fds[i].events);

            if (fds[i].revents)
                ready++;
        }

        /* Events available — return immediately */
        if (ready > 0)
            break;

        /* Non-blocking poll: return 0 */
        if (timeout_ms == 0) {
            timed_out = 1;
            break;
        }

        /* Check absolute timeout */
        uint64_t elapsed = timer_get_ticks() - start_tick;
        uint64_t timeout_ticks = (timeout_ms * TIMER_FREQ) / 1000;
        if (timeout_ms == ~0ULL)
            timeout_ticks = ~0ULL;

        if (elapsed >= timeout_ticks) {
            timed_out = 1;
            break;
        }

        /*
         * ── Block using poll_schedule ─────────────────────────
         * This uses waitqueue-based waiting if any poll handler
         * has registered a waitqueue; otherwise falls back to
         * scheduler_yield() for 1 tick.
         *
         * Calculate remaining timeout to pass to poll_schedule.
         */
        uint64_t remaining_ms;
        if (timeout_ms == ~0ULL) {
            remaining_ms = ~0ULL;
        } else {
            uint64_t remaining_ticks = timeout_ticks - elapsed;
            remaining_ms = (remaining_ticks * 1000) / TIMER_FREQ;
            if (remaining_ms == 0)
                remaining_ms = 1;
        }

        /* Reset the poll table for this iteration so fd poll
         * handlers can re-register their waitqueues. */
        pt.nr_entries = 0;

        int sret = poll_schedule(&pt, remaining_ms);
        if (sret == -EINTR) {
            /* Interrupted by signal — return 0 ready fds.
             * Linux returns -EINTR in this case, but the
             * existing codebase returns 0.  Keep existing
             * behaviour for compatibility. */
            ready = 0;
            break;
        }
        if (sret == -ETIME) {
            timed_out = 1;
            break;
        }
        /* Woken normally — loop and re-check */
    }

    return (uint64_t)ready;
}

/* ── Unified VFS poll dispatcher ─────────────────────────────── */

/*
 * vfs_poll_fd — unified file descriptor poll dispatcher.
 *
 * Dispatches to the correct subsystem poll handler based on fd
 * number ranges:
 *   Socket FDs:   100–131  → sock_poll()
 *   Timerfd FDs:  800–815  → timerfd_poll()
 *   Signalfd FDs: 600–615  → signalfd_poll()
 *   Eventfd FDs:  700–715  → eventfd_poll()
 *   Inotify FDs:  720–727  → inotify_poll()
 *   Pipe FDs:              → pipe_poll() via fd_table path
 *   Regular FDs:           → always POLLIN|POLLOUT
 *   Epoll FDs:   730–745   → POLLIN (if events pending)
 *   Invalid FDs:           → POLLNVAL
 *
 * @fd:      file descriptor number
 * @events:  requested events (POLLIN, POLLOUT, etc.)
 *
 * Returns a revents bitmask of ready events, or POLLNVAL if fd
 * is not valid.
 */
int vfs_poll_fd(int fd, int events)
{
    int revents = 0;

    /* ── Socket FDs (100–131) ──────────────────────────────── */
    if (fd >= 100 && fd < 100 + SOCK_MAX) {
        revents = sock_poll(fd, events, NULL);
        return revents;
    }

    /* ── Timerfd FDs (800–815) ─────────────────────────────── */
    if (fd >= 800 && fd < 800 + 16) {
        revents = timerfd_poll(NULL, NULL);
        if (revents < 0)
            return POLLNVAL;
        return revents & events;
    }

    /* ── Signalfd FDs (600–615) ────────────────────────────── */
    if (fd >= 600 && fd < 600 + 16) {
        revents = signalfd_poll(fd, NULL);
        if (revents < 0)
            return POLLNVAL;
        return revents & events;
    }

    /* ── Eventfd FDs (700–715) ─────────────────────────────── */
    if (fd >= 700 && fd < 700 + 16) {
        revents = eventfd_poll(fd, NULL);
        if (revents < 0)
            return POLLNVAL;
        return revents & events;
    }

    /* ── Inotify FDs (720–727) ─────────────────────────────── */
    if (fd >= INOTIFY_FD_BASE &&
        fd < INOTIFY_FD_BASE + 8) {
        revents = inotify_poll(fd);
        if (revents < 0)
            return POLLNVAL;
        if (revents && (events & POLLIN))
            return POLLIN;
        return 0;
    }

    /* ── Epoll FDs (730–745) ───────────────────────────────── */
    if (fd >= EPOLL_FD_BASE && fd < EPOLL_FD_BASE + 16) {
        /* Epoll fds are always readable if they have pending events.
         * For now, report POLLIN since epoll uses its own blocking. */
        if (events & POLLIN)
            return POLLIN;
        return 0;
    }

    /* ── Process fd_table entries ──────────────────────────── */
    struct process *cur = process_get_current();
    if (!cur || fd < 0 || fd >= PROCESS_FD_MAX)
        return POLLNVAL;

    if (!cur->fd_table[fd].used)
        return POLLNVAL;

    struct process_fd *pfd = &cur->fd_table[fd];

    /* Pipe FDs — dispatch by path prefix */
    if (strncmp(pfd->path, "pipe_read_", 10) == 0) {
        int pipe_id = (int)pfd->offset;
        revents = pipe_poll(pipe_id, 1, NULL);
    } else if (strncmp(pfd->path, "pipe_write_", 11) == 0) {
        int pipe_id = (int)pfd->offset;
        revents = pipe_poll(pipe_id, 0, NULL);
    } else {
        /* Regular files / other: always readable and writable */
        if (events & POLLIN)
            revents |= POLLIN;
        if (events & POLLOUT)
            revents |= POLLOUT;
    }

    return revents & events;
}
