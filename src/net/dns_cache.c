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
 *   - TTL: uses value from DNS reply; falls back to runtime default (300s)
 *   - Periodic timer evicts expired entries every 60s
 *   - TTL jitter (±10%) prevents thundering herd on expiry
 *   - TTL capped at configurable maximum (default 24h)
 *   - Statistics counters for observability
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "dns_cache.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "timers.h"
#include "rng.h"
#include "vfs.h"
#include "net_internal.h"

/* ── Cache state ────────────────────────────────────────────────────── */

static struct dns_cache_entry dns_cache[DNS_CACHE_SIZE];

/* Statistics counters */
static struct dns_cache_stats dns_stats;

/* TTL configuration */
static uint32_t dns_cache_default_ttl     = DNS_CACHE_TTL;
static uint32_t dns_cache_max_ttl         = 86400;   /* 24 hours max */
static uint32_t dns_cache_expiry_interval = 60;      /* scan every 60s */

/* Periodic expiry timer (-1 = not running) */
static int dns_cache_timer_id = -1;

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
 * dns_cache_expiry_cb - Periodic timer callback to evict expired entries.
 * Re-schedules itself at the configured interval.
 */
static void dns_cache_expiry_cb(void *arg)
{
    (void)arg;
    dns_cache_evict_expired();
    /* Re-schedule */
    uint64_t interval_ticks = (uint64_t)dns_cache_expiry_interval * 100;
    dns_cache_timer_id = timer_schedule(dns_cache_expiry_cb, NULL, interval_ticks);
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

    /* Cache is full — evict the least recently used entry */
    int slot = 0;
    uint64_t oldest = dns_cache[0].last_access;
    for (int i = 1; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].last_access < oldest) {
            oldest = dns_cache[i].last_access;
            slot   = i;
        }
    }
    kprintf("[dns_cache] LRU evicting slot %d (last_access=%lu, name=%s)\n",
            slot, (unsigned long)dns_cache[slot].last_access,
            dns_cache[slot].valid ? dns_cache[slot].name : "empty");
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

    /* Start periodic expiry timer */
    if (dns_cache_timer_id < 0) {
        uint64_t interval_ticks = (uint64_t)dns_cache_expiry_interval * 100;
        dns_cache_timer_id = timer_schedule(dns_cache_expiry_cb, NULL, interval_ticks);
        if (dns_cache_timer_id >= 0)
            kprintf("[dns_cache] periodic expiry timer started (interval=%us)\n",
                    dns_cache_expiry_interval);
    }
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
                dns_cache[i].last_access = now;
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
    uint32_t ttl_sec = ttl > 0 ? ttl : dns_cache_default_ttl;

    /* Cap TTL to prevent excessively long-lived entries */
    if (ttl_sec > dns_cache_max_ttl)
        ttl_sec = dns_cache_max_ttl;

    /* Add random jitter (±10% of TTL) to prevent thundering herd
     * when many entries are inserted at the same time.  Entries
     * with tiny TTLs (< 5s) skip jitter since the impact is noise. */
    if (ttl_sec > 5) {
        uint32_t max_jitter = ttl_sec / 10 + 1;
        uint32_t jitter     = rng_get_u32() % max_jitter;
        if (rng_get_u32() & 1) {
            /* Subtract jitter */
            if (ttl_sec > jitter)
                ttl_sec -= jitter;
        } else {
            /* Add jitter (still capped) */
            uint64_t plus_jitter = (uint64_t)ttl_sec + (uint64_t)jitter;
            if (plus_jitter < (uint64_t)dns_cache_max_ttl)
                ttl_sec = (uint32_t)plus_jitter;
        }
    }

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
    dns_cache[slot].last_access = timer_get_ticks();
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
        kprintf("  [%2d] %s -> %u.%u.%u.%u (ttl=%u, expires=%lu, last_access=%lu)\n",
                i, dns_cache[i].name,
                (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
                (unsigned)((ip >>  8) & 0xFF), (unsigned)( ip        & 0xFF),
                dns_cache[i].ttl, (unsigned long)dns_cache[i].expires,
                (unsigned long)dns_cache[i].last_access);
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

/* ── DNS resolver layer with /etc/resolv.conf support ────────────── */

#ifndef DNS_SERVER_MAX
#define DNS_SERVER_MAX    4
#endif
#define DNS_SEARCH_MAX   4
#define DNS_SEARCH_LEN   64

/* Resolver state */
static uint32_t dns_resolv_servers[DNS_SERVER_MAX];
static int      dns_resolv_server_count = 0;
static char     dns_search_domains[DNS_SEARCH_MAX][DNS_SEARCH_LEN];
static int      dns_search_count = 0;

/* ── Parse /etc/resolv.conf ────────────────────────────────────────
 * Reads nameserver lines and search domains from /etc/resolv.conf.
 * Called once during initialization and whenever resolv.conf changes.
 */
void dns_resolver_parse_resolv_conf(void)
{
    char buf[512];
    uint32_t out_size = 0;

    /* Reset state */
    dns_resolv_server_count = 0;
    dns_search_count = 0;

    int ret = vfs_read("/etc/resolv.conf", buf, sizeof(buf) - 1, &out_size);
    if (ret < 0 || out_size == 0) {
        /* No resolv.conf — use defaults */
        if (net_dns_server != 0) {
            dns_resolv_servers[0] = net_dns_server;
            dns_resolv_server_count = 1;
        }
        return;
    }

    buf[out_size] = '\0';

    const char *p = buf;
    while (p && *p && (dns_resolv_server_count < DNS_SERVER_MAX ||
                       dns_search_count < DNS_SEARCH_MAX)) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (strncmp(p, "nameserver", 10) == 0 && dns_resolv_server_count < DNS_SERVER_MAX) {
            p += 10;
            while (*p == ' ' || *p == '\t') p++;

            /* Parse A.B.C.D */
            uint32_t parts[4] = {0};
            int pi = 0;
            const char *start = p;
            while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t' && pi < 4) {
                if (*p >= '0' && *p <= '9')
                    parts[pi] = parts[pi] * 10 + (uint32_t)(*p - '0');
                else if (*p == '.')
                    pi++;
                else
                    break;
                p++;
            }
            if (pi == 3 && p > start) {
                uint32_t ip = (parts[0] << 24) | (parts[1] << 16) |
                              (parts[2] << 8)  | parts[3];
                dns_resolv_servers[dns_resolv_server_count++] = ip;
            }
            /* Skip to end of line */
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        if (strncmp(p, "search", 6) == 0 && dns_search_count < DNS_SEARCH_MAX) {
            p += 6;
            while (*p == ' ' || *p == '\t') p++;

            /* Parse search domains (space-separated) */
            while (*p && *p != '\n' && dns_search_count < DNS_SEARCH_MAX) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p || *p == '\n') break;

                int di = 0;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && di < DNS_SEARCH_LEN - 1)
                    dns_search_domains[dns_search_count][di++] = *p++;
                dns_search_domains[dns_search_count][di] = '\0';
                if (di > 0) dns_search_count++;
            }

            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        /* Skip to end of line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    /* Fallback: if no nameservers found, use global net_dns_server */
    if (dns_resolv_server_count == 0 && net_dns_server != 0) {
        dns_resolv_servers[0] = net_dns_server;
        dns_resolv_server_count = 1;
    }
}

/* ── dns_resolver_server_count ─────────────────────────────────────
 * Returns the number of configured nameservers.
 */
int dns_resolver_server_count(void)
{
    return dns_resolv_server_count;
}

/* ── dns_resolver_server_get ───────────────────────────────────────
 * Returns the IP of the i-th nameserver (0-indexed).
 */
uint32_t dns_resolver_server_get(int i)
{
    if (i < 0 || i >= dns_resolv_server_count) return 0;
    return dns_resolv_servers[i];
}

/* ── dns_resolver_search_count ─────────────────────────────────────
 * Returns the number of search domains.
 */
int dns_resolver_search_count(void)
{
    return dns_search_count;
}

/* ── dns_resolver_search_get ───────────────────────────────────────
 * Returns the i-th search domain string.
 */
const char *dns_resolver_search_get(int i)
{
    if (i < 0 || i >= dns_search_count) return NULL;
    return dns_search_domains[i];
}

/* ── dns_resolver_resolve ──────────────────────────────────────────
 * Try to resolve a hostname.  If the name contains a dot, try as-is first.
 * Otherwise, try appending each search domain.
 * Returns IP in host byte order, or 0 on failure.
 */
uint32_t dns_resolver_resolve(const char *hostname)
{
    uint32_t ip;

    if (!hostname || !*hostname) return 0;

    /* Check cache first with full name */
    ip = dns_cache_lookup(hostname);
    if (ip) return ip;

    /* If name has a dot, try as-is via DNS query (simulated lookup for now) */
    int has_dot = 0;
    for (const char *c = hostname; *c; c++) {
        if (*c == '.') { has_dot = 1; break; }
    }

    if (has_dot) {
        /* In a full implementation, we'd send a DNS query here.
         * For now, return 0 to indicate uncached. */
        return 0;
    }

    /* Try with search domains appended */
    for (int i = 0; i < dns_search_count; i++) {
        char full_name[265]; /* DNS_NAME_MAX + "." + DNS_SEARCH_LEN */
        size_t nlen = strlen(hostname);
        size_t slen = strlen(dns_search_domains[i]);
        if (nlen + 1 + slen >= sizeof(full_name)) continue;

        memcpy(full_name, hostname, nlen);
        full_name[nlen] = '.';
        memcpy(full_name + nlen + 1, dns_search_domains[i], slen);
        full_name[nlen + 1 + slen] = '\0';

        ip = dns_cache_lookup(full_name);
        if (ip) return ip;
    }

    return 0;
}

void net_dns_cache_set(const char *hostname, uint32_t ip) {
    dns_cache_store(hostname, ip, 0);  /* 0 = use runtime default TTL */
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
/* ── Implement: dns_cache_insert ────────────────── */
int dns_cache_insert(const char *name, uint32_t ip, uint32_t ttl)
{
    if (!name || !*name) {
        kprintf("[dns_cache] dns_cache_insert: invalid name\n");
        return -EINVAL;
    }
    if (ip == 0) {
        kprintf("[dns_cache] dns_cache_insert: invalid IP\n");
        return -EINVAL;
    }
    kprintf("[dns_cache] dns_cache_insert: %s -> %u.%u.%u.%u ttl=%u (stub)\n",
            name,
            (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
            (unsigned)((ip >> 8) & 0xFF), (unsigned)(ip & 0xFF),
            ttl);
    /* Fall back to dns_cache_store which already exists */
    dns_cache_store(name, ip, ttl);
    return 0;
}

/* ── Implement: dns_cache_remove ────────────────── */
int dns_cache_remove(const char *name)
{
    if (!name || !*name) {
        kprintf("[dns_cache] dns_cache_remove: invalid name\n");
        return -EINVAL;
    }
    kprintf("[dns_cache] dns_cache_remove: %s (stub)\n", name);
    /* Scan and invalidate matching entries */
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && strcmp(dns_cache[i].name, name) == 0) {
            dns_cache[i].valid = 0;
            dns_stats.entries--;
            return 0;
        }
    }
    return -ENOENT;
}

/* ── Stub: dns_cache_flush ──────────────────────────────────────── */
void dns_cache_flush(void)
{
    kprintf("[dns_cache] dns_cache_flush: clearing all entries\n");
    dns_cache_clear();
}

/* ── Stub: dns_cache_expire ─────────────────────────────────────── */
void dns_cache_expire(void)
{
    kprintf("[dns_cache] dns_cache_expire: evicting expired entries\n");
    dns_cache_evict_expired();
}

/* ── Stub: dns_cache_stats ──────────────────────────────────────── */
struct dns_cache_stats dns_cache_stats(void)
{
    kprintf("[dns_cache] dns_cache_stats: returning current stats\n");
    return dns_cache_get_stats();
}

/* ── Implement: dns_cache_set_size ────────────────── */
int dns_cache_set_size(int new_size)
{
    if (new_size <= 0 || new_size > 1024) {
        kprintf("[dns_cache] dns_cache_set_size: invalid size %d\n", new_size);
        return -EINVAL;
    }
    kprintf("[dns_cache] dns_cache_set_size: new_size=%d (stub — using fixed cache)\n", new_size);
    return -EOPNOTSUPP;
}

/* ── Implement: dns_cache_set_ttl ────────────────── */
uint32_t dns_cache_get_default_ttl(void)
{
    return dns_cache_default_ttl;
}

int dns_cache_set_ttl(uint32_t new_ttl)
{
    if (new_ttl == 0) {
        kprintf("[dns_cache] dns_cache_set_ttl: TTL must be > 0\n");
        return -EINVAL;
    }
    if (new_ttl > dns_cache_max_ttl) {
        kprintf("[dns_cache] dns_cache_set_ttl: TTL %u exceeds max %u, capping\n",
                new_ttl, dns_cache_max_ttl);
        new_ttl = dns_cache_max_ttl;
    }
    dns_cache_default_ttl = new_ttl;
    kprintf("[dns_cache] dns_cache_set_ttl: default TTL set to %u seconds\n", new_ttl);
    return 0;
}

/* ── dns_cache_get_max_ttl / dns_cache_set_max_ttl ── */
uint32_t dns_cache_get_max_ttl(void)
{
    return dns_cache_max_ttl;
}

int dns_cache_set_max_ttl(uint32_t new_max)
{
    if (new_max < 1 || new_max > 86400 * 365) {
        kprintf("[dns_cache] dns_cache_set_max_ttl: invalid max TTL %u\n", new_max);
        return -EINVAL;
    }
    if (new_max < dns_cache_default_ttl) {
        kprintf("[dns_cache] dns_cache_set_max_ttl: max %u < default %u, "
                "clamping default\n", new_max, dns_cache_default_ttl);
        dns_cache_default_ttl = new_max;
    }
    dns_cache_max_ttl = new_max;
    kprintf("[dns_cache] dns_cache_set_max_ttl: max TTL set to %u seconds\n", new_max);
    return 0;
}

/* ── dns_cache_set_expiry_interval / dns_cache_get_expiry_interval ── */
uint32_t dns_cache_get_expiry_interval(void)
{
    return dns_cache_expiry_interval;
}

int dns_cache_set_expiry_interval(uint32_t new_interval)
{
    if (new_interval < 1 || new_interval > 86400) {
        kprintf("[dns_cache] dns_cache_set_expiry_interval: invalid interval %u\n",
                new_interval);
        return -EINVAL;
    }
    dns_cache_expiry_interval = new_interval;

    /* Restart the timer with the new interval */
    if (dns_cache_timer_id >= 0) {
        timer_cancel(dns_cache_timer_id);
        dns_cache_timer_id = -1;
    }
    uint64_t interval_ticks = (uint64_t)dns_cache_expiry_interval * 100;
    dns_cache_timer_id = timer_schedule(dns_cache_expiry_cb, NULL, interval_ticks);
    kprintf("[dns_cache] dns_cache_set_expiry_interval: interval set to %u seconds\n",
            new_interval);
    return 0;
}

/* ── dns_cache_get_expired_count ─────────────────── */
int dns_cache_get_expired_count(void)
{
    return dns_stats.expired;
}

/* ── dns_cache_stop_expiry_timer ─────────────────── */
void dns_cache_stop_expiry_timer(void)
{
    if (dns_cache_timer_id >= 0) {
        timer_cancel(dns_cache_timer_id);
        dns_cache_timer_id = -1;
        kprintf("[dns_cache] periodic expiry timer stopped\n");
    }
}
#include "module.h"
module_init(net_dns_cache_init);
