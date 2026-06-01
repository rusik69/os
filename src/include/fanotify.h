#ifndef FANOTIFY_H
#define FANOTIFY_H

#include "types.h"

/* Event mask flags — which events are reported */
#define FAN_ACCESS      0x01   /* file was accessed (read) */
#define FAN_MODIFY      0x02   /* file was modified (write) */
#define FAN_OPEN        0x04   /* file was opened */
#define FAN_CLOSE_WRITE 0x08   /* file opened for writing was closed */

/* Maximum notification groups */
#define FAN_MAX_GROUPS 8

/* Maximum events per group ring buffer */
#define FAN_EVENT_RING_SIZE 64

/* Maximum marks per group */
#define FAN_MAX_MARKS 32

/* Path length limit stored in events */
#define FAN_PATH_MAX 128

/* Fanotify event — delivered to userspace via the notification fd */
struct fanotify_event {
    int      used;
    uint64_t mask;              /* FAN_ACCESS | FAN_MODIFY | ... */
    char     path[FAN_PATH_MAX];
};

/* Per-mark entry (a watched path + mask) */
struct fanotify_mark {
    int      in_use;
    char     path[FAN_PATH_MAX];
    uint64_t mask;
};

/* Per-group state */
struct fanotify_group {
    int      in_use;
    int      flags;
    int      event_f_flags;
    /* Event ring buffer (FIFO) */
    struct fanotify_event events[FAN_EVENT_RING_SIZE];
    int      event_head;
    int      event_tail;
    int      event_count;
    /* Mark table */
    struct fanotify_mark marks[FAN_MAX_MARKS];
    int      mark_count;
};

/* Create a new fanotify notification group.
 * flags: FAN_CLOEXEC, FAN_NONBLOCK, etc.
 * event_f_flags: O_RDONLY, O_RDWR, etc. for the fanotify fd.
 * Returns a file descriptor (group index) on success, negative on error. */
int fanotify_create_group(int flags, int event_f_flags);

/* Add/remove/override a watch mark.
 * fd: the fanotify fd from fanotify_create_group().
 * flags: FAN_MARK_ADD, FAN_MARK_REMOVE, FAN_MARK_FLUSH, etc.
 * mask: bitmask of FAN_* events to watch.
 * dirfd: AT_FDCWD or a directory fd (unused in this simplified model).
 * path: path to watch.
 * Returns 0 on success, negative on error. */
int fanotify_mark(int fd, int flags, uint64_t mask, int dirfd, const char *path);

/* Read the next event from the fanotify group.
 * fd: fanotify fd.
 * evt: pointer to a fanotify_event struct to fill.
 * sz: size of the event buffer (must be >= sizeof(struct fanotify_event)).
 * Returns bytes written on success, 0 if no events (non-blocking), negative on error. */
int fanotify_get_event(int fd, struct fanotify_event *evt, size_t sz);

/* Called by the VFS layer to report an access event.
 * path: the path of the file being accessed.
 * mask: the type of access (FAN_ACCESS, FAN_MODIFY, etc.).
 * This function dispatches to all groups that have a matching mark. */
void fanotify_handle_event(const char *path, uint64_t mask);

/* Initialise the fanotify subsystem. */
void fanotify_init(void);

#endif /* FANOTIFY_H */
