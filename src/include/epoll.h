#ifndef EPOLL_H
#define EPOLL_H

/*
 * epoll.h — Kernel-internal epoll interface
 *
 * Provides I/O event notification facility (epoll).
 * The epoll instance is a global-slot-based subsystem (fd range 730..745)
 * following the same pattern as eventfd, signalfd, and timerfd.
 *
 * Lifecycle:
 *   1. epoll_create1(flags) → epoll fd
 *   2. epoll_ctl(epfd, op, fd, event) — add/mod/del monitored fds
 *   3. epoll_wait(epfd, events, maxevents, timeout) — wait for events
 *   4. epoll_close(epfd) — release the instance
 */

#include "types.h"
#include "waitqueue.h"
#include "errno.h"

/* ── Forward declarations ─────────────────────────────────────── */

struct epoll_event;

/* ── Constants ───────────────────────────────────────────────── */

/* Maximum epoll instances per system */
#define EPOLL_MAX          16

/* Maximum events per epoll instance */
#define EPOLL_MAX_EVENTS   64

/* Epoll fd base — fd numbers 730..745 are epoll instances */
#define EPOLL_FD_BASE      730

/* epoll_create1 flags */
#define EPOLL_CLOEXEC      02000000

/* ── Data structures ─────────────────────────────────────────── */

/*
 * epoll_fd_entry — a single fd being monitored by an epoll instance.
 *
 * @fd:            file descriptor being monitored
 * @events:        requested epoll events (EPOLLIN, EPOLLOUT, etc.)
 * @data:          user-provided data returned on event delivery
 * @last_reported: last set of events reported to user (for EPOLLET delta)
 * @in_use:        1 if this slot is active
 * @armed:         1 if actively monitoring (0 = disarmed by EPOLLONESHOT)
 */
struct epoll_fd_entry {
    int      fd;
    uint32_t events;
    uint64_t data;
    uint32_t last_reported;
    int      in_use;
    uint8_t  armed;
};

/*
 * epoll_instance — per-epoll_create1() instance.
 *
 * @in_use:       1 if this instance is active
 * @flags:        creation flags (EPOLL_CLOEXEC)
 * @entries:      array of monitored fds
 * @num_entries:  number of active entries
 * @wq:           wait queue for epoll_wait blockers
 */
struct epoll_instance {
    int    in_use;
    int    flags;
    struct epoll_fd_entry entries[EPOLL_MAX_EVENTS];
    int    num_entries;
    struct wait_queue wq;
};

/* ── Kernel-internal API ─────────────────────────────────────── */

/*
 * epoll_create1_syscall — create an epoll instance.
 *
 * @flags:  creation flags (EPOLL_CLOEXEC or 0)
 *
 * Returns the epoll fd number on success, or a negative errno:
 *   -EINVAL  if invalid flags are set
 *   -ENFILE  if no epoll instances available
 */
int epoll_create1_syscall(int flags);

/*
 * epoll_ctl_syscall — control interface for an epoll instance.
 *
 * @epfd:   epoll fd from epoll_create1
 * @op:     EPOLL_CTL_ADD, EPOLL_CTL_MOD, or EPOLL_CTL_DEL
 * @fd:     target file descriptor
 * @event:  pointer to struct epoll_event with events and data
 *
 * Returns 0 on success, or a negative errno:
 *   -EBADF  if epfd is not a valid epoll instance
 *   -EINVAL if op is invalid or bad parameters
 *   -ENOENT if fd not found (for DEL/MOD)
 *   -ENOMEM if instance is full (for ADD)
 */
int epoll_ctl_syscall(int epfd, int op, int fd,
                       struct epoll_event *event);

/*
 * epoll_wait_syscall — wait for events on an epoll instance.
 *
 * @epfd:       epoll fd
 * @events:     user-space buffer for ready events
 * @maxevents:  size of the events buffer
 * @timeout:    milliseconds to wait (-1 = infinite)
 *
 * Returns the number of ready events on success, 0 on timeout,
 * or a negative errno:
 *   -EBADF  if epfd is not a valid epoll instance
 *   -EFAULT if the events buffer is not writable
 *   -EINTR  if interrupted by a signal
 */
int epoll_wait_syscall(int epfd, struct epoll_event *events,
                        int maxevents, int timeout);

/*
 * epoll_close — release an epoll instance.
 */
void epoll_close(int epfd);

/*
 * epoll_pwait_syscall — epoll_wait with signal mask.
 *
 * Like epoll_wait_syscall, but additionally applies @sigmask
 * during the wait.  Currently a compatibility stub that ignores
 * the signal mask.
 */
int epoll_pwait_syscall(int epfd, struct epoll_event *events,
                         int maxevents, int timeout,
                         const uint64_t *sigmask);

#endif /* EPOLL_H */
