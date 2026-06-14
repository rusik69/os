#ifndef DNS_CACHE_H
#define DNS_CACHE_H

#include "types.h"

/* ── Configuration ─────────────────────────────────────────────────── */

#define DNS_CACHE_SIZE   32       /* max entries in cache */
#define DNS_CACHE_TTL    300      /* default TTL (seconds) if server omits it */
#define DNS_NAME_MAX     255      /* max length for a hostname */

/* ── Statistics ────────────────────────────────────────────────────── */

struct dns_cache_stats {
    int      entries;            /* current number of valid entries */
    int      capacity;           /* maximum capacity */
    uint32_t lookups;            /* total lookup operations */
    uint32_t hits;               /* cache hit count (valid+unexpired) */
    uint32_t misses;             /* cache miss count (not found) */
    uint32_t stores;             /* total store operations */
    uint32_t expired;            /* entries evicted due to TTL expiry */
    uint32_t evictions;          /* entries evicted due to cache full */
};

/* ── Per-entry TTL tracking ────────────────────────────────────────── */

struct dns_cache_entry {
    char     name[DNS_NAME_MAX]; /* hostname (null-terminated) */
    uint32_t ip;                 /* IP address in host byte order */
    uint64_t expires;            /* absolute tick when this entry expires */
    uint32_t ttl;                /* original TTL in seconds (from DNS reply) */
    int      valid;              /* 1 = slot occupied */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * dns_cache_init - Initialize the DNS cache (clear all entries)
 */
void dns_cache_init(void);

/**
 * dns_cache_lookup - Look up a hostname in the cache
 * @name: hostname to look up (null-terminated)
 * Returns: IP in host byte order, or 0 if not found or expired
 */
uint32_t dns_cache_lookup(const char *name);

/**
 * dns_cache_store - Store a DNS resolution result in the cache
 * @name:   hostname (null-terminated)
 * @ip:     IP address in host byte order
 * @ttl:    time-to-live in seconds (from DNS reply; 0 = use default)
 */
void dns_cache_store(const char *name, uint32_t ip, uint32_t ttl);

/**
 * dns_cache_clear - Remove all entries from the cache
 */
void dns_cache_clear(void);

/**
 * dns_cache_get_stats - Get current cache statistics
 */
struct dns_cache_stats dns_cache_get_stats(void);

/**
 * dns_cache_dump - Print all cache entries to kernel log (debug helper)
 */
void dns_cache_dump(void);

/**
 * dns_cache_foreach - Iterate over valid cache entries
 * @callback: called for each valid entry; return non-zero to stop early
 * @arg:      opaque pointer passed to callback
 */
void dns_cache_foreach(int (*callback)(const struct dns_cache_entry *e, void *arg), void *arg);

/* ── Compatibility wrappers (exported via net.h / net.c) ───────────── */

/* These are used by existing consumers */
void          net_dns_cache_set(const char *hostname, uint32_t ip);
uint32_t      net_dns_cache_get(const char *hostname);
void          net_dns_cache_clear(void);
/* Extended API for observability */
struct dns_cache_stats net_dns_cache_stats(void);
void          net_dns_cache_dump(void);
void          net_dns_cache_init(void);

/* ── DNS resolver layer ──────────────────────────────────────────── */
void     dns_resolver_parse_resolv_conf(void);
int      dns_resolver_server_count(void);
uint32_t dns_resolver_server_get(int i);
int      dns_resolver_search_count(void);
const char *dns_resolver_search_get(int i);
uint32_t dns_resolver_resolve(const char *hostname);

#endif /* DNS_CACHE_H */
