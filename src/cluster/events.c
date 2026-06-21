/*
 * events.c — Cluster event bus (publish/subscribe with history replay)
 *
 * Provides a publish/subscribe model for cluster-wide events.
 * Components can subscribe to event types and receive notifications.
 * Supports event history replay for late joiners.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "heap.h"
#include "errno.h"
#include "timer.h"

/* ── Constants ─────────────────────────────────────────────────────────── */

#define EVENTS_MAX_SUBSCRIBERS   64
#define EVENTS_HISTORY_DEPTH     128
#define EVENTS_NAME_MAX          48
#define EVENTS_DATA_MAX          256

/* ── Event types ───────────────────────────────────────────────────────── */

#define EVENT_NODE_JOINED        1
#define EVENT_NODE_LEFT          2
#define EVENT_NODE_UNHEALTHY     3
#define EVENT_POD_STARTED        4
#define EVENT_POD_STOPPED        5
#define EVENT_POD_CRASHED        6
#define EVENT_SERVICE_UPDATE     7
#define EVENT_CONFIG_CHANGE      8
#define EVENT_LEADER_ELECTION    9
#define EVENT_CUSTOM            100

/* ── Event entry ───────────────────────────────────────────────────────── */

struct event_entry {
    int      in_use;
    uint64_t id;                  /* monotonic event ID */
    uint64_t timestamp;           /* when event occurred (ms) */
    int      type;                /* EVENT_* type */
    char     source[EVENTS_NAME_MAX];  /* source component */
    uint8_t  data[EVENTS_DATA_MAX];
    uint32_t data_len;
};

/* ── Subscriber ────────────────────────────────────────────────────────── */

struct event_subscriber {
    int      in_use;
    int      id;                  /* subscriber ID */
    uint32_t event_mask;          /* bitmask of event types subscribed to */
    uint64_t last_event_id;       /* last event received (for history replay) */
    int      (*callback)(struct event_entry *ev, void *ctx);
    void     *ctx;
    char     name[EVENTS_NAME_MAX];
};

/* ── Global state ──────────────────────────────────────────────────────── */

static struct event_entry   g_history[EVENTS_HISTORY_DEPTH];
static int                  g_history_count;
static uint64_t             g_next_event_id;
static struct event_subscriber g_subscribers[EVENTS_MAX_SUBSCRIBERS];
static int                  g_subscriber_count;
static spinlock_t           g_events_lock;
static int                  g_events_initialised;

/* ── Initialisation ────────────────────────────────────────────────────── */

void cluster_events_init(void)
{
    if (g_events_initialised)
        return;

    memset(g_history, 0, sizeof(g_history));
    memset(g_subscribers, 0, sizeof(g_subscribers));
    g_history_count = 0;
    g_next_event_id = 1;
    g_subscriber_count = 0;
    spinlock_init(&g_events_lock);
    g_events_initialised = 1;

    kprintf("[events] Cluster event bus initialised (%d history, %d subscribers)\n",
            EVENTS_HISTORY_DEPTH, EVENTS_MAX_SUBSCRIBERS);
}

/* ── Event publishing ──────────────────────────────────────────────────── */

uint64_t cluster_event_publish(int type, const char *source,
                                const void *data, uint32_t data_len)
{
    if (!g_events_initialised || !source)
        return 0;

    if (type < 1)
        return 0;

    if (data_len > EVENTS_DATA_MAX)
        data_len = EVENTS_DATA_MAX;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_events_lock, &irq_flags);

    uint64_t event_id = g_next_event_id++;

    /* Store in history ring buffer */
    int hist_idx = g_history_count % EVENTS_HISTORY_DEPTH;
    struct event_entry *ev = &g_history[hist_idx];
    ev->in_use    = 1;
    ev->id        = event_id;
    ev->timestamp = timer_get_ms();
    ev->type      = type;
    strncpy(ev->source, source, EVENTS_NAME_MAX - 1);
    ev->source[EVENTS_NAME_MAX - 1] = '\0';
    ev->data_len  = data_len;
    if (data && data_len > 0)
        memcpy(ev->data, data, data_len);
    g_history_count++;

    /* Notify matching subscribers */
    for (int i = 0; i < EVENTS_MAX_SUBSCRIBERS; i++) {
        struct event_subscriber *sub = &g_subscribers[i];
        if (!sub->in_use || !sub->callback)
            continue;

        /* Check if subscriber cares about this event type */
        uint32_t type_bit = (uint32_t)(1u << (type - 1));
        if (type >= 32 || !(sub->event_mask & type_bit))
            continue;

        sub->last_event_id = event_id;

        /* Release lock during callback to avoid deadlock */
        spinlock_irqsave_release(&g_events_lock, irq_flags);
        sub->callback(ev, sub->ctx);
        spinlock_irqsave_acquire(&g_events_lock, &irq_flags);
    }

    spinlock_irqsave_release(&g_events_lock, irq_flags);
    return event_id;
}

/* ── Subscriber management ─────────────────────────────────────────────── */

int cluster_event_subscribe(const char *name, uint32_t event_mask,
                             int (*callback)(struct event_entry *ev, void *ctx),
                             void *ctx, int replay_history)
{
    if (!g_events_initialised || !name || !callback)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_events_lock, &irq_flags);

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < EVENTS_MAX_SUBSCRIBERS; i++) {
        if (!g_subscribers[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_events_lock, irq_flags);
        return -ENOMEM;
    }

    struct event_subscriber *sub = &g_subscribers[slot];
    sub->in_use       = 1;
    sub->id           = slot + 1;
    sub->event_mask   = event_mask;
    sub->last_event_id = 0;
    sub->callback     = callback;
    sub->ctx          = ctx;
    strncpy(sub->name, name, EVENTS_NAME_MAX - 1);
    sub->name[EVENTS_NAME_MAX - 1] = '\0';
    g_subscriber_count++;

    /* Replay history for late joiner */
    if (replay_history && g_history_count > 0) {
        int start;
        if (g_history_count > EVENTS_HISTORY_DEPTH) {
            /* Ring buffer has wrapped — start from oldest entry */
            start = g_history_count % EVENTS_HISTORY_DEPTH;
        } else {
            start = 0;
        }

        int count = (g_history_count < EVENTS_HISTORY_DEPTH)
                     ? g_history_count : EVENTS_HISTORY_DEPTH;

        for (int i = 0; i < count; i++) {
            int idx = (start + i) % EVENTS_HISTORY_DEPTH;
            struct event_entry *ev = &g_history[idx];
            if (!ev->in_use)
                continue;

            uint32_t type_bit = (uint32_t)(1u << (ev->type - 1));
            if (ev->type >= 32 || !(event_mask & type_bit))
                continue;

            sub->last_event_id = ev->id;

            /* Release lock during callback */
            spinlock_irqsave_release(&g_events_lock, irq_flags);
            callback(ev, ctx);
            spinlock_irqsave_acquire(&g_events_lock, &irq_flags);
        }
    }

    spinlock_irqsave_release(&g_events_lock, irq_flags);

    kprintf("[events] Subscriber '%s' registered (mask=0x%x, replay=%d)\n",
            name, event_mask, replay_history);
    return sub->id;
}

int cluster_event_unsubscribe(int subscriber_id)
{
    if (!g_events_initialised)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_events_lock, &irq_flags);

    for (int i = 0; i < EVENTS_MAX_SUBSCRIBERS; i++) {
        if (g_subscribers[i].in_use && g_subscribers[i].id == subscriber_id) {
            g_subscribers[i].in_use = 0;
            g_subscribers[i].callback = NULL;
            g_subscriber_count--;
            spinlock_irqsave_release(&g_events_lock, irq_flags);
            kprintf("[events] Subscriber %d unregistered\n", subscriber_id);
            return 0;
        }
    }

    spinlock_irqsave_release(&g_events_lock, irq_flags);
    return -ENOENT;
}

/* ── Query functions ───────────────────────────────────────────────────── */

int cluster_event_get_history(struct event_entry *buf, int max_events,
                               int start_type)
{
    if (!g_events_initialised || !buf || max_events <= 0)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_events_lock, &irq_flags);

    int copied = 0;
    if (g_history_count > EVENTS_HISTORY_DEPTH) {
        int start = g_history_count % EVENTS_HISTORY_DEPTH;
        for (int i = 0; i < EVENTS_HISTORY_DEPTH && copied < max_events; i++) {
            int idx = (start + i) % EVENTS_HISTORY_DEPTH;
            if (g_history[idx].in_use &&
                (start_type == 0 || g_history[idx].type == start_type)) {
                memcpy(&buf[copied], &g_history[idx], sizeof(struct event_entry));
                copied++;
            }
        }
    } else {
        for (int i = 0; i < g_history_count && copied < max_events; i++) {
            if (g_history[i].in_use &&
                (start_type == 0 || g_history[i].type == start_type)) {
                memcpy(&buf[copied], &g_history[i], sizeof(struct event_entry));
                copied++;
            }
        }
    }

    spinlock_irqsave_release(&g_events_lock, irq_flags);
    return copied;
}

uint64_t cluster_event_get_count(void)
{
    return g_events_initialised ? (uint64_t)g_history_count : 0;
}

/* ── Stub: event_post ─────────────────────────────── */
int event_post(const char *type, const char *reason, const char *msg)
{
    (void)type;
    (void)reason;
    (void)msg;
    kprintf("[cluster] event_post: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: event_list ─────────────────────────────── */
int event_list(void *events)
{
    (void)events;
    kprintf("[cluster] event_list: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: event_watch ─────────────────────────────── */
int event_watch(const char *type, void *callback)
{
    (void)type;
    (void)callback;
    kprintf("[cluster] event_watch: not yet implemented\n");
    return -ENOSYS;
}
