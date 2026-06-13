#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include "types.h"

/* DNS server port */
#define DNS_SERVER_PORT 53

/* Resource record types */
#define DNS_TYPE_A      1
#define DNS_TYPE_AAAA   28
#define DNS_TYPE_CNAME  5
#define DNS_TYPE_MX     15
#define DNS_TYPE_SOA    6
#define DNS_TYPE_NS     2

/* DNS class (IN = Internet) */
#define DNS_CLASS_IN    1

/* Maximum zone file size and records */
#define DNS_MAX_ZONE_SIZE      4096
#define DNS_MAX_RECORDS        64
#define DNS_NAME_MAX_LEN       256

/* DNS resource record */
struct dns_rr {
    char     name[DNS_NAME_MAX_LEN];  /* owner name */
    uint16_t type;                      /* A, AAAA, CNAME, MX, etc. */
    uint16_t rr_class;                  /* usually IN (1) */
    uint32_t ttl;                       /* time to live (seconds) */
    /* Data (type-dependent) */
    union {
        uint32_t a_record;              /* A: IPv4 address (network byte order) */
        uint8_t  aaaa_record[16];       /* AAAA: IPv6 address (16 bytes) */
        char     cname[DNS_NAME_MAX_LEN]; /* CNAME: canonical name */
        struct {
            uint16_t preference;
            char     exchange[DNS_NAME_MAX_LEN];
        } mx;                           /* MX: mail exchange */
        struct {
            char     mname[DNS_NAME_MAX_LEN];
            char     rname[DNS_NAME_MAX_LEN];
            uint32_t serial;
            uint32_t refresh;
            uint32_t retry;
            uint32_t expire;
            uint32_t minimum;
        } soa;                          /* SOA: start of authority */
    } data;
};

/**
 * dns_server_init - Initialize the DNS authoritative server
 *
 * Parses zone file /etc/dns_zones.txt and starts listening on UDP 53.
 * Forwards unresolved queries to the upstream DNS server.
 *
 * Returns 0 on success, -1 on error.
 */
int dns_server_init(void);

/**
 * dns_server_stop - Stop the DNS server and free resources
 */
void dns_server_stop(void);

#endif /* DNS_SERVER_H */
