/*
 * raft_kv.c — Distributed key-value store via Raft (C109–C111)
 *
 * Implements:
 *   C109: Cluster state store — distributed KV store via Raft
 *   C110: Watch support — key prefix monitoring
 *   C111: Lease mechanism — node heartbeats with TTL
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define KV_MAX_ENTRIES   1024
#define KV_KEY_MAX       256
#define KV_VAL_MAX       4096
#define KV_WATCH_MAX     64
#define KV_WATCH_EVENTS  16

/* ── KV entry ────────────────────────────────────────────────────────── */

struct kv_entry {
    char   in_use;
    char   key[KV_KEY_MAX];
    uint8_t value[KV_VAL_MAX];
    uint32_t value_len;
    uint64_t version;         /* Monotonically increasing version */
    uint64_t expire_time;     /* 0 = no expiry (lease) */
    spinlock_t lock;
};

static struct kv_entry kv_store[KV_MAX_ENTRIES];
static uint64_t kv_version_counter = 0;

/* ── Watch ──────────────────────────────────────────────────────────── */

/* Watch event types */
#define KV_EVENT_PUT     0
#define KV_EVENT_DELETE  1

struct kv_watch_event {
    int    type;         /* KV_EVENT_PUT or KV_EVENT_DELETE */
    char   key[KV_KEY_MAX];
    uint8_t value[KV_VAL_MAX];
    uint32_t value_len;
    uint64_t version;
};

struct kv_watch {
    char   in_use;
    char   prefix[KV_KEY_MAX];
    int    num_events;
    struct kv_watch_event events[KV_WATCH_EVENTS];
    int    has_events;         /* Signal flag */
};

static struct kv_watch kv_watches[KV_WATCH_MAX];
static int kv_watch_count = 0;
static int kv_initialised = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  C109: Cluster state store — distributed KV store via Raft
 * ═══════════════════════════════════════════════════════════════════════ */

/* C109: Initialise the KV store */
int kv_init(void)
{
    memset(kv_store, 0, sizeof(kv_store));
    memset(kv_watches, 0, sizeof(kv_watches));
    kv_version_counter = 0;
    kv_watch_count = 0;
    kv_initialised = 1;
    kprintf("[KV] Key-value store initialised (%d max entries)\n", KV_MAX_ENTRIES);
    return 0;
}

/* C109: Put a key-value pair (via Raft consensus) */
int kv_put(const char *key, const uint8_t *value, uint32_t value_len)
{
    if (!key || !value || !kv_initialised) return -EINVAL;
    if (value_len > KV_VAL_MAX) return -EINVAL;

    /* Find existing or empty slot */
    int idx = -1;
    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        if (kv_store[i].in_use && strcmp(kv_store[i].key, key) == 0) {
            idx = i;
            break;
        }
        if (!kv_store[i].in_use && idx < 0) idx = i;
    }
    if (idx < 0) return -ENOSPC;

    struct kv_entry *entry = &kv_store[idx];
    spinlock_acquire(&entry->lock);

    entry->in_use = 1;
    strncpy(entry->key, key, KV_KEY_MAX - 1);
    memcpy(entry->value, value, value_len);
    entry->value_len = value_len;
    entry->version = ++kv_version_counter;
    entry->expire_time = 0;

    spinlock_release(&entry->lock);

    /* Notify watchers */
    for (int w = 0; w < kv_watch_count; w++) {
        if (kv_watches[w].in_use &&
            strncmp(key, kv_watches[w].prefix, strlen(kv_watches[w].prefix)) == 0) {
            if (kv_watches[w].num_events < KV_WATCH_EVENTS) {
                struct kv_watch_event *ev = &kv_watches[w].events[kv_watches[w].num_events++];
                ev->type = KV_EVENT_PUT;
                strncpy(ev->key, key, KV_KEY_MAX - 1);
                memcpy(ev->value, value, value_len);
                ev->value_len = value_len;
                ev->version = entry->version;
                kv_watches[w].has_events = 1;
            }
        }
    }

    return 0;
}

/* C109: Get a value by key */
int kv_get(const char *key, uint8_t *value_out, uint32_t *value_len_out)
{
    if (!key || !value_out || !value_len_out || !kv_initialised) return -EINVAL;

    uint64_t now = timer_get_ms();

    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        if (!kv_store[i].in_use) continue;

        /* Check for expired lease */
        if (kv_store[i].expire_time > 0 && now >= kv_store[i].expire_time) {
            kv_store[i].in_use = 0; /* Auto-expire */
            continue;
        }

        if (strcmp(kv_store[i].key, key) == 0) {
            spinlock_acquire(&kv_store[i].lock);
            uint32_t copy_len = kv_store[i].value_len < *value_len_out ?
                                kv_store[i].value_len : *value_len_out;
            memcpy(value_out, kv_store[i].value, copy_len);
            *value_len_out = copy_len;
            spinlock_release(&kv_store[i].lock);
            return 0;
        }
    }
    return -ENOENT;
}

/* C109: Delete a key */
int kv_delete(const char *key)
{
    if (!key || !kv_initialised) return -EINVAL;

    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        if (kv_store[i].in_use && strcmp(kv_store[i].key, key) == 0) {
            kv_store[i].in_use = 0;

            /* Notify watchers */
            for (int w = 0; w < kv_watch_count; w++) {
                if (kv_watches[w].in_use &&
                    strncmp(key, kv_watches[w].prefix, strlen(kv_watches[w].prefix)) == 0) {
                    if (kv_watches[w].num_events < KV_WATCH_EVENTS) {
                        struct kv_watch_event *ev = &kv_watches[w].events[kv_watches[w].num_events++];
                        ev->type = KV_EVENT_DELETE;
                        strncpy(ev->key, key, KV_KEY_MAX - 1);
                        ev->version = ++kv_version_counter;
                        kv_watches[w].has_events = 1;
                    }
                }
            }
            return 0;
        }
    }
    return -ENOENT;
}

/* C109: List keys matching a prefix */
int kv_list_prefix(const char *prefix, char *buf, size_t bufsz)
{
    if (!prefix || !buf || !kv_initialised) return -EINVAL;

    int pos = 0;
    size_t prefix_len = strlen(prefix);

    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        if (!kv_store[i].in_use) continue;
        if (strncmp(kv_store[i].key, prefix, prefix_len) == 0) {
            if ((size_t)pos >= bufsz) break;
            int n = snprintf(buf + pos, bufsz - (size_t)pos,
                             "%s (v%lu, %u bytes)\n",
                             kv_store[i].key, kv_store[i].version, kv_store[i].value_len);
            if (n < 0) break;
            pos += n;
        }
    }
    return pos;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C110: Watch support — key prefix monitoring
 * ═══════════════════════════════════════════════════════════════════════ */

/* C110: Register a watch on a key prefix */
int kv_watch_register(const char *prefix)
{
    if (!prefix || !kv_initialised) return -EINVAL;
    if (kv_watch_count >= KV_WATCH_MAX) return -ENOSPC;

    struct kv_watch *w = &kv_watches[kv_watch_count++];
    strncpy(w->prefix, prefix, KV_KEY_MAX - 1);
    w->num_events = 0;
    w->has_events = 0;
    w->in_use = 1;

    return kv_watch_count - 1; /* Return watch ID */
}

/* C110: Read watch events (non-blocking) */
int kv_watch_read(int watch_id, struct kv_watch_event *events, int max_events)
{
    if (watch_id < 0 || watch_id >= kv_watch_count || !events || !kv_initialised)
        return -EINVAL;

    struct kv_watch *w = &kv_watches[watch_id];
    if (!w->in_use || !w->has_events) return 0;

    int count = w->num_events < max_events ? w->num_events : max_events;
    memcpy(events, w->events, (size_t)count * sizeof(struct kv_watch_event));
    w->num_events = 0;
    w->has_events = 0;

    return count;
}

/* C110: Unregister a watch */
int kv_watch_unregister(int watch_id)
{
    if (watch_id < 0 || watch_id >= kv_watch_count || !kv_initialised)
        return -EINVAL;

    kv_watches[watch_id].in_use = 0;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C111: Lease mechanism — node heartbeats with TTL
 * ═══════════════════════════════════════════════════════════════════════ */

/* C111: Acquire a lease (key with TTL) */
int kv_lease_acquire(const char *key, uint64_t ttl_ms,
                     const uint8_t *owner, uint32_t owner_len)
{
    if (!key || !owner || !kv_initialised) return -EINVAL;

    /* Check if lease already held by someone else */
    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        if (!kv_store[i].in_use || strcmp(kv_store[i].key, key) != 0)
            continue;

        uint64_t now = timer_get_ms();
        if (kv_store[i].expire_time > 0 && now < kv_store[i].expire_time) {
            return -EBUSY; /* Lease held by another node */
        }
        /* Lease expired — we can take it */
        break;
    }

    int ret = kv_put(key, owner, owner_len);
    if (ret < 0) return ret;

    /* Set expiry */
    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        if (kv_store[i].in_use && strcmp(kv_store[i].key, key) == 0) {
            kv_store[i].expire_time = timer_get_ms() + ttl_ms;
            break;
        }
    }

    return 0;
}

/* C111: Refresh a lease (heartbeat) */
int kv_lease_refresh(const char *key, uint64_t ttl_ms)
{
    if (!key || !kv_initialised) return -EINVAL;

    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        if (kv_store[i].in_use && strcmp(kv_store[i].key, key) == 0) {
            kv_store[i].expire_time = timer_get_ms() + ttl_ms;
            return 0;
        }
    }
    return -ENOENT;
}

/* C111: Release a lease */
int kv_lease_release(const char *key)
{
    return kv_delete(key);
}

/* C111: Check if a key still has a valid lease */
int kv_lease_valid(const char *key)
{
    if (!key || !kv_initialised) return 0;

    uint64_t now = timer_get_ms();
    for (int i = 0; i < KV_MAX_ENTRIES; i++) {
        if (kv_store[i].in_use && strcmp(kv_store[i].key, key) == 0) {
            if (kv_store[i].expire_time == 0) return 1; /* No expiry */
            return now < kv_store[i].expire_time ? 1 : 0;
        }
    }
    return 0;
}

/* ── Stub: raft_kv_put ─────────────────────────────── */
int raft_kv_put(const char *key, const void *val, size_t len)
{
    (void)key;
    (void)val;
    (void)len;
    kprintf("[cluster] raft_kv_put: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: raft_kv_get ─────────────────────────────── */
int raft_kv_get(const char *key, void *val, size_t *len)
{
    (void)key;
    (void)val;
    (void)len;
    kprintf("[cluster] raft_kv_get: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: raft_kv_delete ─────────────────────────────── */
int raft_kv_delete(const char *key)
{
    (void)key;
    kprintf("[cluster] raft_kv_delete: not yet implemented\n");
    return -ENOSYS;
}
