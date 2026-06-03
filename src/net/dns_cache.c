/*
 * dns_cache.c — DNS caching stub resolver
 *
 * Implements a per-entry TTL-aware DNS cache with LRU eviction,
 * statistics tracking, and a clean public API.
 *
 * This replaces the earlier inline cache in net_udp.c with a
 * proper standalone module that uses actual DNS TTL values from
 * server responses.
 *
 * Design:
 *   - Fixed-size array (DNS_CACHE_SIZE = 32 entries)
 *   - Lookup: linear scan with TTL expiry check
 *   - Store: update existing entry, or LRU-evict oldest
 *   - TTL: uses value from DNS reply; falls back to DNS_CACHE_TTL (300s)
 *   - Statistics counters for observability
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "dns_cache.h"
#include "string.h"
#include "printf.h"
#include "timer.h"

/* ── Cache state ────────────────────────────────────────────────────── */

static struct dns_cache_entry dns_cache[DNS_CACHE_SIZE];

/* Statistics counters */
static struct dns_cache_stats dns_stats;

/* ── Internal helpers ───────────────────────────────────────────────── */

/**
 * dns_cache_evict_expired - Scan and mark expired entries as invalid.
 * Called by lookup and store to keep the cache clean.
 */
static void dns_cache_evict_expired(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && now >= dns_cache[i].expires) {
            dns_cache[i].valid = 0;
            dns_stats.entries--;
            dns_stats.expired++;
        }
    }
}

/**
 * dns_cache_find_slot - Find a slot for storing a new entry.
 * Priority:
 *   1. Existing entry with matching name (update)
 *   2. Empty/invalid slot
 *   3. LRU: entry closest to expiry
 */
static int dns_cache_find_slot(const char *name) {
    /* First pass: look for existing matching name */
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && strcmp(dns_cache[i].name, name) == 0)
            return i;
    }

    /* Evict any expired entries first to free slots */
    dns_cache_evict_expired();

    /* Second pass: find an invalid slot */
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid)
            return i;
    }

    /* Cache is full — find the entry closest to expiry (LRU-evict) */
    int slot = 0;
    uint64_t oldest = dns_cache[0].expires;
    for (int i = 1; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].expires < oldest) {
            oldest = dns_cache[i].expires;
            slot   = i;
        }
    }
    dns_cache[slot].valid = 0;
    dns_stats.entries--;
    dns_stats.evictions++;
    return slot;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void dns_cache_init(void) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++)
        dns_cache[i].valid = 0;
    memset(&dns_stats, 0, sizeof(dns_stats));
    dns_stats.capacity = DNS_CACHE_SIZE;
}

uint32_t dns_cache_lookup(const char *name) {
    if (!name || !*name) return 0;

    dns_stats.lookups++;

    /* Evict stale entries opportunistically */
    dns_cache_evict_expired();

    uint64_t now = timer_get_ticks();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && strcmp(dns_cache[i].name, name) == 0) {
            if (now < dns_cache[i].expires) {
                dns_stats.hits++;
                return dns_cache[i].ip;
            }
            /* Expired — mark as invalid */
            dns_cache[i].valid = 0;
            dns_stats.entries--;
            dns_stats.expired++;
            break;
        }
    }

    dns_stats.misses++;
    return 0;
}

void dns_cache_store(const char *name, uint32_t ip, uint32_t ttl) {
    if (!name || !*name || !ip) return;

    dns_stats.stores++;

    /* Compute expiry time */
    uint32_t ttl_sec = ttl > 0 ? ttl : DNS_CACHE_TTL;
    /* Convert seconds to ticks (timers use centiseconds ~10ms at 100Hz) */
    uint64_t ttl_ticks = (uint64_t)ttl_sec * 100;  /* 100 Hz timer */
    uint64_t expires   = timer_get_ticks() + ttl_ticks;

    /* Handle wraparound: if timer wraps, clamp to max */
    if (expires < timer_get_ticks() && ttl_ticks > 0)
        expires = (uint64_t)-1;  /* basically never expires */

    int slot = dns_cache_find_slot(name);
    int was_valid = dns_cache[slot].valid;

    /* Fill the slot */
    size_t len = strlen(name);
    if (len >= DNS_NAME_MAX) len = DNS_NAME_MAX - 1;
    memcpy(dns_cache[slot].name, name, len);
    dns_cache[slot].name[len] = '\0';
    dns_cache[slot].ip        = ip;
    dns_cache[slot].ttl       = ttl_sec;
    dns_cache[slot].expires   = expires;
    if (!was_valid)
        dns_stats.entries++;
    dns_cache[slot].valid     = 1;
}

void dns_cache_clear(void) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++)
        dns_cache[i].valid = 0;
    dns_stats.entries    = 0;
    dns_stats.expired    = 0;
    dns_stats.evictions  = 0;
    dns_stats.stores     = 0;
    dns_stats.hits       = 0;
    dns_stats.misses     = 0;
    dns_stats.lookups    = 0;
}

struct dns_cache_stats dns_cache_get_stats(void) {
    /* Re-count valid entries for accuracy */
    int count = 0;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid) count++;
    }
    dns_stats.entries = count;
    return dns_stats;
}

void dns_cache_dump(void) {
    kprintf("[dns_cache] %d/%d entries, %u hits, %u misses, %u stores, "
            "%u expired, %u evictions\n",
            dns_stats.entries, dns_stats.capacity,
            dns_stats.hits, dns_stats.misses, dns_stats.stores,
            dns_stats.expired, dns_stats.evictions);
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid) continue;
        uint32_t ip = dns_cache[i].ip;
        kprintf("  [%2d] %s -> %u.%u.%u.%u (ttl=%u, expires=%lu)\n",
                i, dns_cache[i].name,
                (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
                (unsigned)((ip >>  8) & 0xFF), (unsigned)( ip        & 0xFF),
                dns_cache[i].ttl, (unsigned long)dns_cache[i].expires);
    }
}

void dns_cache_foreach(int (*callback)(const struct dns_cache_entry *e, void *arg), void *arg) {
    if (!callback) return;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid) continue;
        if (callback(&dns_cache[i], arg))
            break;
    }
}

/* ── Compatibility wrappers ────────────────────────────────────────── */

void net_dns_cache_set(const char *hostname, uint32_t ip) {
    dns_cache_store(hostname, ip, DNS_CACHE_TTL);
}

uint32_t net_dns_cache_get(const char *hostname) {
    return dns_cache_lookup(hostname);
}

void net_dns_cache_clear(void) {
    dns_cache_clear();
}

struct dns_cache_stats net_dns_cache_stats(void) {
    return dns_cache_get_stats();
}

void net_dns_cache_dump(void) {
    dns_cache_dump();
}

void net_dns_cache_init(void) {
    dns_cache_init();
}
