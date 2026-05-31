#ifndef FSNOTIFY_H
#define FSNOTIFY_H

#include "types.h"

#define FSNOTIFY_MAX_WATCHES 8

/* Event types (bitmask) */
#define FS_CREATE 1
#define FS_DELETE 2
#define FS_MODIFY 4
#define FS_ACCESS 8

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

#endif /* FSNOTIFY_H */
