#ifndef DNS_RESOLVER_H
#define DNS_RESOLVER_H

#include "types.h"
#include "net.h"

/* ── DNS resolver configuration ────────────────────────────────────── */

#define DNS_RESOLVER_SRC_PORT  1054    /* source port for DNS queries */
#define DNS_TIMEOUT_TICKS      300     /* 3 seconds at 100 Hz timer */
#define DNS_RETRIES            3       /* number of query retries */

/* ── DNS record types (RFC 1035) ────────────────────────────────────── */
#define DNS_TYPE_A      1
#define DNS_TYPE_AAAA   28
#define DNS_TYPE_CNAME  5
#define DNS_TYPE_SRV    33
#define DNS_TYPE_PTR    12

#define DNS_CLASS_IN    1

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * dns_resolver_init - Initialize the DNS resolver module
 *
 * Seeds the transaction ID counter.  Must be called once during
 * network stack initialization (usually from net_init).
 *
 * Returns 0 on success.
 */
int dns_resolver_init(void);

/**
 * dns_resolver_query_a - Perform DNS A record lookup over UDP (port 53)
 *
 * Sends a DNS A-record query to the configured DNS server, waits for
 * the response, parses it, caches the result, and returns the IPv4
 * address in host byte order.
 *
 * @hostname: hostname to resolve (null-terminated)
 * @out_ip:   receives resolved IPv4 address (host byte order)
 * @out_ttl:  receives TTL in seconds from DNS reply (may be NULL)
 *
 * Returns: 0 on success, negative errno on failure
 *   -EHOSTUNREACH  no DNS server configured
 *   -ENOENT        domain name does not exist (NXDOMAIN)
 *   -ETIMEDOUT     no response after all retries
 *   -EIO           server returned an error
 */
int dns_resolver_query_a(const char *hostname, uint32_t *out_ip, uint32_t *out_ttl);

/**
 * dns_resolver_query_aaaa - Perform DNS AAAA record lookup over UDP (port 53)
 *
 * Sends a DNS AAAA-record query to the configured IPv6 DNS server via
 * UDP over IPv6, waits for the response, parses it, and returns the
 * IPv6 address.
 *
 * @hostname: hostname to resolve (null-terminated)
 * @out_addr: receives resolved IPv6 address (16 bytes, network byte order)
 * @out_ttl:  receives TTL in seconds from DNS reply (may be NULL)
 *
 * Returns: 0 on success, negative errno on failure
 *   -EHOSTUNREACH  no IPv6 DNS server configured
 *   -ENOENT        domain name does not exist (NXDOMAIN)
 *   -ETIMEDOUT     no response after all retries
 *   -EIO           server returned an error
 */
int dns_resolver_query_aaaa(const char *hostname, struct in6_addr *out_addr,
                            uint32_t *out_ttl);

#endif /* DNS_RESOLVER_H */
