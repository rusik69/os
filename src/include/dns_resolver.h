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

/* ── CNAME resolution chain ─────────────────────────────────────────── */
#define DNS_CNAME_MAX_CHAIN  5   /* max CNAME follow depth */

/* ── SRV record (RFC 2782) ──────────────────────────────────────────── */

/**
 * struct dns_srv_record - A single SRV record (RFC 2782)
 * @priority: lower values = higher priority
 * @weight:   relative weight among same-priority records
 * @port:     TCP/UDP port of the service
 * @target:   canonical hostname providing the service
 */
struct dns_srv_record {
    uint16_t priority;
    uint16_t weight;
    uint16_t port;
    char target[256];       /* DNS name string, max 255 chars + NUL */
};

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

/**
 * dns_resolver_query_a_follow_cname - Query A record following CNAME chain
 *
 * Like dns_resolver_query_a, but when the server returns only a CNAME
 * (no A record), automatically follows the CNAME chain up to
 * DNS_CNAME_MAX_CHAIN (5) levels deep.  Each step sends a new A-record
 * query for the canonical name.
 *
 * @hostname: hostname to resolve (null-terminated)
 * @out_ip:   receives resolved IPv4 address (host byte order)
 * @out_ttl:  receives TTL in seconds from DNS reply (may be NULL)
 *
 * Returns: 0 on success, negative errno on failure
 *   -EHOSTUNREACH  no DNS server configured
 *   -ENOENT        domain name does not exist (NXDOMAIN)
 *   -ELOOP         CNAME chain exceeded maximum depth
 *   -ETIMEDOUT     no response after all retries
 *   -EIO           server returned an error
 */
int dns_resolver_query_a_follow_cname(const char *hostname,
                                       uint32_t *out_ip, uint32_t *out_ttl);

/**
 * dns_resolver_query_srv - Perform DNS SRV record lookup (RFC 2782)
 *
 * Sends a DNS SRV-record query to the configured DNS server, waits for
 * the response, and parses all SRV records in the answer section.
 * SRV records are used for service discovery — e.g., querying
 * "_sip._tcp.example.com" returns the SIP servers for example.com.
 *
 * @hostname:   service name to query (e.g. "_sip._tcp.example.com")
 * @records:    array to receive SRV records
 * @count:      on input: max records; on output: number of records found
 * @out_ttl:    receives TTL in seconds (may be NULL)
 *
 * Returns: 0 on success, negative errno on failure
 *   -EHOSTUNREACH  no DNS server configured
 *   -ENOENT        domain name does not exist (NXDOMAIN)
 *   -ETIMEDOUT     no response after all retries
 *   -EIO           server returned an error or malformed response
 *   -ENOSPC        more SRV records than @count allows
 */
int dns_resolver_query_srv(const char *hostname, struct dns_srv_record *records,
                           int *count, uint32_t *out_ttl);

/**
 * dns_resolver_query_ptr_ipv4 - Perform DNS PTR record reverse lookup (IPv4)
 *
 * Converts the IPv4 address to the in-addr.arpa format and performs a
 * PTR record query (type 12) against the configured DNS server.
 * The resolved hostname is written to @out_hostname.
 *
 * @ip:         IPv4 address in host byte order
 * @out_hostname: buffer to receive the resolved hostname (null-terminated)
 * @out_max:    size of @out_hostname buffer
 * @out_ttl:    receives TTL in seconds from DNS reply (may be NULL)
 *
 * Returns: 0 on success, negative errno on failure
 *   -EHOSTUNREACH  no DNS server configured
 *   -ENOENT        no PTR record found (NXDOMAIN)
 *   -ETIMEDOUT     no response after all retries
 *   -ENOSPC        output buffer too small for hostname
 *   -EIO           server returned an error or malformed response
 */
int dns_resolver_query_ptr_ipv4(uint32_t ip, char *out_hostname,
                                int out_max, uint32_t *out_ttl);

#endif /* DNS_RESOLVER_H */
