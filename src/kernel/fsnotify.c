/*
 * fsnotify.c — Simple filesystem notification subsystem
 *
 * Provides an inotify-like mechanism for monitoring file/directory events.
 * Supports up to FSNOTIFY_MAX_WATCHES concurrent watches.
 * Events are logged to a small ring buffer.
 */

#include "fsnotify.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

/* Forward declaration — inotify delivery hook (defined in inotify.c) */
void inotify_deliver(const char *path, uint32_t fs_mask);

#define FSNOTIFY_EVENT_RING 32

static struct {
    char     path[64];
    uint32_t mask;
    int      in_use;
} g_watches[FSNOTIFY_MAX_WATCHES];

static struct fsnotify_event g_event_ring[FSNOTIFY_EVENT_RING];
static int g_event_head = 0;
static int g_event_count = 0;

static spinlock_t g_fsn_lock;
static int g_fsn_initialized = 0;

void fsnotify_init(void) {
    memset(g_watches, 0, sizeof(g_watches));
    memset(g_event_ring, 0, sizeof(g_event_ring));
    spinlock_init(&g_fsn_lock);
    g_fsn_initialized = 1;
    kprintf("[OK] FS notify initialized (%d watches)\n", FSNOTIFY_MAX_WATCHES);
}

int fsnotify_watch(const char *path, uint32_t mask) {
    if (!path || !g_fsn_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_fsn_lock, &irq_flags);

    int slot = -1;
    for (int i = 0; i < FSNOTIFY_MAX_WATCHES; i++) {
        if (!g_watches[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_fsn_lock, irq_flags);
        return -1;
    }

    strncpy(g_watches[slot].path, path, 63);
    g_watches[slot].path[63] = '\0';
    g_watches[slot].mask = mask;
    g_watches[slot].in_use = 1;

    spinlock_irqsave_release(&g_fsn_lock, irq_flags);
    return slot;
}

void fsnotify_unwatch(int watch_id) {
    if (watch_id < 0 || watch_id >= FSNOTIFY_MAX_WATCHES || !g_fsn_initialized) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_fsn_lock, &irq_flags);
    g_watches[watch_id].in_use = 0;
    g_watches[watch_id].mask = 0;
    spinlock_irqsave_release(&g_fsn_lock, irq_flags);
}

void fsnotify_notify(const char *path, uint32_t event) {
    if (!path || !g_fsn_initialized) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_fsn_lock, &irq_flags);

    /* Check all watchers — if anyone watches this path with matching mask, log event */
    int matched = 0;
    for (int i = 0; i < FSNOTIFY_MAX_WATCHES; i++) {
        if (!g_watches[i].in_use) continue;
        if (!(g_watches[i].mask & event)) continue;
        /* Simple prefix match: the watched path is a prefix of the event path */
        if (strncmp(g_watches[i].path, path, strlen(g_watches[i].path)) == 0) {
            matched = 1;
            break;
        }
    }

    if (matched) {
        /* Deliver to inotify instances */
        inotify_deliver(path, event);

        /* Store event in ring buffer */
        int idx = g_event_head;
        strncpy(g_event_ring[idx].path, path, 63);
        g_event_ring[idx].path[63] = '\0';
        g_event_ring[idx].mask = event;

        g_event_head = (g_event_head + 1) % FSNOTIFY_EVENT_RING;
        if (g_event_count < FSNOTIFY_EVENT_RING)
            g_event_count++;
    }

    spinlock_irqsave_release(&g_fsn_lock, irq_flags);
}

int fsnotify_read_events(struct fsnotify_event *events, int max) {
    if (!events || max <= 0 || !g_fsn_initialized) return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_fsn_lock, &irq_flags);

    int nread = 0;
    /* Read from oldest to newest */
    int start = (g_event_head - g_event_count + FSNOTIFY_EVENT_RING) % FSNOTIFY_EVENT_RING;
    for (int i = 0; i < g_event_count && nread < max; i++) {
        int idx = (start + i) % FSNOTIFY_EVENT_RING;
        events[nread] = g_event_ring[idx];
        nread++;
    }

    /* Clear read events */
    g_event_count = 0;

    spinlock_irqsave_release(&g_fsn_lock, irq_flags);
    return nread;
}
