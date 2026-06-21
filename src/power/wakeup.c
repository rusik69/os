/*
 * wakeup.c — Power Management wakeup event source tracking.
 *
 * Implements a framework for tracking wakeup-capable devices and their
 * event counts.  The PM suspend path uses this to determine whether it
 * is safe to enter a sleep state: if any wakeup event is still being
 * processed (active flag set), suspend is deferred to avoid losing the
 * event.  After resume, wakeup_clear_active() acknowledges all events.
 *
 * Each wakeup source is identified by a small integer ID (its index in
 * the fixed-size table).  Registration succeeds as long as there is a
 * free slot.
 *
 * Reference: Linux kernel drivers/base/power/wakeup.c
 */
#define KERNEL_INTERNAL
#include "types.h"
#include "wakeup.h"
#include "spinlock.h"
#include "string.h"
#include "printf.h"
#include "errno.h"

/* ── Internal data structures ──────────────────────────────────────── */

/** Per-source flags. */
#define WSF_IN_USE     0x01  /* Slot is occupied */
#define WSF_ACTIVE     0x02  /* Event is in-flight (blocks suspend) */

/** A single wakeup source descriptor. */
struct wakeup_source {
    char      name[WAKEUP_NAME_MAX];  /* Human-readable name */
    uint64_t  event_count;            /* Total wakeup events delivered */
    uint8_t   flags;                  /* WSF_* flags */
};

/** Global state. */
static struct {
    struct wakeup_source sources[WAKEUP_SRC_MAX];
    spinlock_t           lock;
    int                  initialized;
} wakeup_state;

/* ══════════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Find a free slot in the wakeup sources table.
 * Must be called with the lock held.
 * Returns the slot index, or -1 if no slots are free.
 */
static int wakeup_find_free_slot_locked(void)
{
    for (int i = 0; i < WAKEUP_SRC_MAX; i++) {
        if (!(wakeup_state.sources[i].flags & WSF_IN_USE))
            return i;
    }
    return -1;
}

/**
 * Validate a source ID.
 * Returns 1 if the ID refers to an in-use source, 0 otherwise.
 */
static int wakeup_id_valid(int id)
{
    return (id >= 0 && id < WAKEUP_SRC_MAX &&
            (wakeup_state.sources[id].flags & WSF_IN_USE));
}

/* ══════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════ */

void wakeup_init(void)
{
    if (wakeup_state.initialized)
        return;

    memset(&wakeup_state, 0, sizeof(wakeup_state));
    spinlock_init(&wakeup_state.lock);
    wakeup_state.initialized = 1;

    kprintf("[wakeup] Wakeup sources subsystem initialized"
            " (max %d sources)\n", WAKEUP_SRC_MAX);
}

int wakeup_source_register(const char *name)
{
    if (!wakeup_state.initialized)
        return -ENODEV;
    if (!name || !name[0])
        return -EINVAL;

    spinlock_acquire(&wakeup_state.lock);

    int slot = wakeup_find_free_slot_locked();
    if (slot < 0) {
        spinlock_release(&wakeup_state.lock);
        kprintf("[wakeup] ERROR: No free wakeup source slots (max %d)\n",
                WAKEUP_SRC_MAX);
        return -ENOSPC;
    }

    struct wakeup_source *ws = &wakeup_state.sources[slot];
    strncpy(ws->name, name, WAKEUP_NAME_MAX - 1);
    ws->name[WAKEUP_NAME_MAX - 1] = '\0';
    ws->event_count = 0;
    ws->flags       = WSF_IN_USE;       /* no event yet */

    spinlock_release(&wakeup_state.lock);

    kprintf("[wakeup] Source #%d: \"%s\" registered\n", slot, name);
    return slot;
}

void wakeup_source_unregister(int id)
{
    if (!wakeup_state.initialized)
        return;

    spinlock_acquire(&wakeup_state.lock);

    if (!wakeup_id_valid(id)) {
        spinlock_release(&wakeup_state.lock);
        return;
    }

    struct wakeup_source *ws = &wakeup_state.sources[id];
    kprintf("[wakeup] Source #%d: \"%s\" unregistered"
            " (total events: %llu)\n",
            id, ws->name,
            (unsigned long long)ws->event_count);

    memset(ws, 0, sizeof(*ws));
    spinlock_release(&wakeup_state.lock);
}

void wakeup_source_event(int id)
{
    if (!wakeup_state.initialized)
        return;

    spinlock_acquire(&wakeup_state.lock);

    if (!wakeup_id_valid(id)) {
        spinlock_release(&wakeup_state.lock);
        return;
    }

    struct wakeup_source *ws = &wakeup_state.sources[id];
    ws->event_count++;
    ws->flags |= WSF_ACTIVE;  /* mark as having an in-flight event */

    spinlock_release(&wakeup_state.lock);
}

uint64_t wakeup_source_count(int id)
{
    if (!wakeup_state.initialized)
        return 0;

    spinlock_acquire(&wakeup_state.lock);

    uint64_t count = 0;
    if (wakeup_id_valid(id))
        count = wakeup_state.sources[id].event_count;

    spinlock_release(&wakeup_state.lock);
    return count;
}

const char *wakeup_source_name(int id)
{
    if (!wakeup_state.initialized)
        return NULL;

    spinlock_acquire(&wakeup_state.lock);

    const char *name = NULL;
    if (wakeup_id_valid(id))
        name = wakeup_state.sources[id].name;

    spinlock_release(&wakeup_state.lock);
    return name;
}

int wakeup_source_is_active(int id)
{
    if (!wakeup_state.initialized)
        return 0;

    spinlock_acquire(&wakeup_state.lock);

    int active = 0;
    if (wakeup_id_valid(id))
        active = (wakeup_state.sources[id].flags & WSF_ACTIVE) ? 1 : 0;

    spinlock_release(&wakeup_state.lock);
    return active;
}

int wakeup_get_active_count(void)
{
    if (!wakeup_state.initialized)
        return 0;

    spinlock_acquire(&wakeup_state.lock);

    int count = 0;
    for (int i = 0; i < WAKEUP_SRC_MAX; i++) {
        if (wakeup_state.sources[i].flags & WSF_ACTIVE)
            count++;
    }

    spinlock_release(&wakeup_state.lock);
    return count;
}

void wakeup_clear_active(void)
{
    if (!wakeup_state.initialized)
        return;

    spinlock_acquire(&wakeup_state.lock);

    for (int i = 0; i < WAKEUP_SRC_MAX; i++) {
        if (wakeup_state.sources[i].flags & WSF_IN_USE)
            wakeup_state.sources[i].flags &= ~(uint8_t)WSF_ACTIVE;
    }

    spinlock_release(&wakeup_state.lock);
}

void wakeup_print_sources(void)
{
    if (!wakeup_state.initialized) {
        kprintf("[wakeup] Subsystem not initialised\n");
        return;
    }

    spinlock_acquire(&wakeup_state.lock);

    int active_count = 0;
    int total_sources = 0;

    kprintf("[wakeup] Wakeup sources:\n");
    kprintf("  %-4s %-32s %-14s %s\n",
            "ID", "Name", "Events", "Status");
    kprintf("  %-4s %-32s %-14s %s\n",
            "----", "--------------------------------",
            "--------------", "------");

    for (int i = 0; i < WAKEUP_SRC_MAX; i++) {
        if (!(wakeup_state.sources[i].flags & WSF_IN_USE))
            continue;

        total_sources++;
        if (wakeup_state.sources[i].flags & WSF_ACTIVE)
            active_count++;

        kprintf("  %-4d %-32s %-14llu %s\n",
                i,
                wakeup_state.sources[i].name,
                (unsigned long long)wakeup_state.sources[i].event_count,
                (wakeup_state.sources[i].flags & WSF_ACTIVE)
                    ? "active" : "idle");
    }

    kprintf("  Total: %d source(s), %d active\n",
            total_sources, active_count);

    spinlock_release(&wakeup_state.lock);
}

/* ── wakeup_source_create ─────────────────────────────── */
int wakeup_source_create(const char *name)
{
    /* Allocate a wakeup source by registering with the base API.
     * Returns the wakeup source ID (>= 0) on success. */
    return wakeup_source_register(name);
}
/* ── wakeup_source_destroy ─────────────────────────────── */
int wakeup_source_destroy(void *ws)
{
    /* Destroy/free a wakeup source.
     * @ws is expected to be the wakeup source ID as a pointer. */
    int id = (int)(uintptr_t)ws;
    return wakeup_source_unregister(id);
}
/* ── wakeup_source_report ─────────────────────────────── */
int wakeup_source_report(void *ws, uint64_t duration)
{
    (void)duration;
    /* Report a wakeup event on the given wakeup source.
     * This is called after handling the wakeup interrupt. */
    int id = (int)(uintptr_t)ws;
    if (wakeup_id_valid(id)) {
        wakeup_state.sources[id].event_count++;
    }
    return 0;
}
