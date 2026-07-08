#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "kernel.h"
#include "fanotify.h"
#include "vfs.h"
#include "errno.h"

/* FAN_MARK_* flags (simplified) */
#define FAN_MARK_ADD      0x01
#define FAN_MARK_REMOVE   0x02
#define FAN_MARK_FLUSH    0x04

/* Static fanotify group table */
static struct fanotify_group fanotify_groups[FAN_MAX_GROUPS];
static int fanotify_initialised = 0;

void fanotify_init(void)
{
    if (fanotify_initialised)
        return;

    memset(fanotify_groups, 0, sizeof(fanotify_groups));
    fanotify_initialised = 1;
    kprintf("[OK] fanotify: fanotify initialised (%d groups, ring %d, marks %d)\n",
            FAN_MAX_GROUPS, FAN_EVENT_RING_SIZE, FAN_MAX_MARKS);
}

/* Find a free group slot */
static int group_find_free(void)
{
    for (int i = 0; i < FAN_MAX_GROUPS; i++) {
        if (!fanotify_groups[i].in_use)
            return i;
    }
    return -ENFILE;
}

/* Check if a group fd is valid */
static struct fanotify_group *group_from_fd(int fd)
{
    if (fd < 0 || fd >= FAN_MAX_GROUPS)
        return NULL;
    struct fanotify_group *g = &fanotify_groups[fd];
    if (!g->in_use)
        return NULL;
    return g;
}

/* Push an event onto a group's ring buffer */
static int event_push(struct fanotify_group *g, const char *path, uint64_t mask)
{
    if (g->event_count >= FAN_EVENT_RING_SIZE)
        return -ENOSPC;

    struct fanotify_event *evt = &g->events[g->event_head];
    evt->used = 1;
    evt->mask = mask;
    strncpy(evt->path, path, FAN_PATH_MAX - 1);
    evt->path[FAN_PATH_MAX - 1] = '\0';

    g->event_head = (g->event_head + 1) % FAN_EVENT_RING_SIZE;
    g->event_count++;
    return 0;
}

/* Pop an event from the ring */
static int event_pop(struct fanotify_group *g, struct fanotify_event *evt)
{
    if (g->event_count == 0)
        return 0;

    struct fanotify_event *src = &g->events[g->event_tail];
    *evt = *src;
    src->used = 0;

    g->event_tail = (g->event_tail + 1) % FAN_EVENT_RING_SIZE;
    g->event_count--;
    return sizeof(struct fanotify_event);
}

/* Create a new fanotify notification group */
int fanotify_create_group(int flags, int event_f_flags)
{
    (void)flags;
    (void)event_f_flags;

    int idx = group_find_free();
    if (idx < 0)
        return idx;

    struct fanotify_group *g = &fanotify_groups[idx];
    g->in_use = 1;
    g->flags = flags;
    g->event_f_flags = event_f_flags;
    g->event_head = 0;
    g->event_tail = 0;
    g->event_count = 0;
    g->mark_count = 0;
    memset(g->events, 0, sizeof(g->events));
    memset(g->marks, 0, sizeof(g->marks));

    return idx; /* return group index as fd */
}

static int mark_add(struct fanotify_group *g, const char *path, uint64_t mask)
{
    if (g->mark_count >= FAN_MAX_MARKS)
        return -ENOSPC;

    /* Check for duplicate */
    for (int i = 0; i < FAN_MAX_MARKS; i++) {
        if (g->marks[i].in_use && strcmp(g->marks[i].path, path) == 0) {
            /* Update existing mark mask */
            g->marks[i].mask = mask;
            return 0;
        }
    }

    struct fanotify_mark *m = &g->marks[g->mark_count];
    m->in_use = 1;
    strncpy(m->path, path, FAN_PATH_MAX - 1);
    m->path[FAN_PATH_MAX - 1] = '\0';
    m->mask = mask;
    g->mark_count++;
    return 0;
}

static int mark_remove(struct fanotify_group *g, const char *path)
{
    for (int i = 0; i < FAN_MAX_MARKS; i++) {
        if (g->marks[i].in_use && strcmp(g->marks[i].path, path) == 0) {
            g->marks[i].in_use = 0;
            g->mark_count--;
            return 0;
        }
    }
    return -ENOENT;
}

int fanotify_mark(int fd, int flags, uint64_t mask, int dirfd, const char *path)
{
    (void)dirfd;

    if (!path)
        return -EINVAL;

    struct fanotify_group *g = group_from_fd(fd);
    if (!g)
        return -EBADF;

    if (flags & FAN_MARK_FLUSH) {
        /* Remove all marks */
        memset(g->marks, 0, sizeof(g->marks));
        g->mark_count = 0;
        return 0;
    }

    if (flags & FAN_MARK_ADD)
        return mark_add(g, path, mask);
    if (flags & FAN_MARK_REMOVE)
        return mark_remove(g, path);

    return -EINVAL;
}

int fanotify_get_event(int fd, struct fanotify_event *evt, size_t sz)
{
    if (!evt || sz < sizeof(struct fanotify_event))
        return -EINVAL;

    struct fanotify_group *g = group_from_fd(fd);
    if (!g)
        return -EBADF;

    return event_pop(g, evt);
}

void fanotify_handle_event(const char *path, uint64_t mask)
{
    if (!fanotify_initialised || !path)
        return;

    /* Dispatch to all groups that have a matching mark */
    for (int g_idx = 0; g_idx < FAN_MAX_GROUPS; g_idx++) {
        struct fanotify_group *g = &fanotify_groups[g_idx];
        if (!g->in_use)
            continue;

        /* Check if any mark matches this path */
        for (int m_idx = 0; m_idx < FAN_MAX_MARKS; m_idx++) {
            struct fanotify_mark *m = &g->marks[m_idx];
            if (!m->in_use)
                continue;

            /* Check if mark path is a prefix of this path */
            size_t mlen = strlen(m->path);
            if (strncmp(m->path, path, mlen) != 0)
                continue;

            /* Check if the event mask matches */
            if (m->mask & mask) {
                event_push(g, path, mask);
                break; /* one event per group is enough */
            }
        }
    }
}

/* ── Stub: fanotify_read ─────────────────────────────── */
static int fanotify_read(int fd, void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    (void)count;
    kprintf("[fanotify] fanotify_read: not yet implemented\n");
    return 0;
}
