#ifndef FSNOTIFY_H
#define FSNOTIFY_H

#include "types.h"

/* ── In-kernel fsnotify (internal) ───────────────────────────── */

#define FSNOTIFY_MAX_WATCHES 8

/* Event types (bitmask) */
#define FS_CREATE   1
#define FS_DELETE   2
#define FS_MODIFY   4
#define FS_ACCESS   8

/* Watch a path for events matching the given mask. Returns watch_id (>=0) or -1. */
int fsnotify_watch(const char *path, uint32_t mask);

/* Remove a watch by its watch_id. */
void fsnotify_unwatch(int watch_id);

/* Notify all watchers that an event occurred on the given path. */
void fsnotify_notify(const char *path, uint32_t event);

/* Initialize the filesystem notification subsystem. */
void fsnotify_init(void);

/* Ring-buffer event entry for reading pending events */
struct fsnotify_event {
    char     path[64];
    uint32_t mask;
};

/* Read events from the notification ring buffer. Returns number read. */
int fsnotify_read_events(struct fsnotify_event *events, int max);

/* ── Inotify userspace API (Item 340) ─────────────────────────── */

#define INOTIFY_FD_BASE    720   /* base fd for inotify instances */
#define INOTIFY_INSTANCES  8     /* max simultaneous inotify fds */

/* inotify_init1 flags */
#define IN_NONBLOCK  0x800
#define IN_CLOEXEC   0x1000000

/* inotify events that userspace can watch for */
#define IN_ACCESS      0x00000001
#define IN_MODIFY      0x00000002
#define IN_ATTRIB      0x00000004
#define IN_CLOSE_WRITE 0x00000008
#define IN_CLOSE_NOWR  0x00000010
#define IN_CLOSE       (IN_CLOSE_WRITE | IN_CLOSE_NOWR)
#define IN_OPEN        0x00000020
#define IN_MOVED_FROM  0x00000040
#define IN_MOVED_TO    0x00000080
#define IN_MOVE        (IN_MOVED_FROM | IN_MOVED_TO)
#define IN_CREATE      0x00000100
#define IN_DELETE      0x00000200
#define IN_DELETE_SELF 0x00000400
#define IN_MOVE_SELF   0x00000800

/* inotify event struct returned by read() on an inotify fd */
struct inotify_event {
    int      wd;       /* watch descriptor */
    uint32_t mask;     /* mask of events */
    uint32_t cookie;   /* unique cookie for related events (e.g. move) */
    uint32_t len;      /* size of name[] field */
    char     name[];   /* optional null-terminated name */
};

/* Convert an in-kernel fsnotify event mask to inotify event mask */
uint32_t fsnotify_to_inotify_mask(uint32_t fsnotify_mask);

#endif /* FSNOTIFY_H */
