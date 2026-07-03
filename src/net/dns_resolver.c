/*
 * dns_resolver.c — DNS resolver (RFC 1035)
 *
 * Implements DNS A record queries over UDP (port 53).  This module
 * sends DNS queries to the configured nameserver, parses responses,
 * and caches results via dns_cache_store().
 *
 * Integration:
 *   - Sends queries via net_udp_send() (src port DNS_RESOLVER_SRC_PORT)
 *   - Receives responses via net_udp_listen() / net_udp_recv()
 *   - Requires handle_udp() in net_udp.c to NOT unconditionally intercept
 *     all DNS_PORT (53) source packets — see the dst_port check there.
 *
 * Thread safety:
 *   - Transaction IDs are atomically incremented.
 *   - Each query call is synchronous (blocks waiting for its own response),
 *     so multiple calls from different threads are safe as long as each
 *     uses its own txid.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "dns_resolver.h"
#include "dns_cache.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "errno.h"

/* ── DNS wire format structures ─────────────────────────────────────── */

/* DNS message header (RFC 1035 §4.1.1) — 12 bytes */
struct __attribute__((packed)) dns_header {
    uint16_t id;          /* transaction ID */
    uint16_t flags;       /* QR, Opcode, AA, TC, RD, RA, Z, RCODE */
    uint16_t qdcount;     /* number of questions */
    uint16_t ancount;     /* number of answer RRs */
    uint16_t nscount;     /* number of authority RRs */
    uint16_t arcount;     /* number of additional RRs */
};

/* DNS header flags */
#define DNS_QR_QUERY    0x0000
#define DNS_QR_RESPONSE 0x8000
#define DNS_OPCODE_STD  0x0000
#define DNS_RD          0x0100   /* Recursion Desired */
#define DNS_RA          0x0080   /* Recursion Available */
#define DNS_RCODE_OK    0x0000
#define DNS_RCODE_FMT   0x0001   /* Format error */
#define DNS_RCODE_SRV   0x0002   /* Server failure */
#define DNS_RCODE_NX    0x0003   /* Non-existent domain */
#define DNS_RCODE_REF   0x0004   /* Refused */

/* ── Transaction tracking ───────────────────────────────────────────── */

static uint16_t dns_txid_counter = 0x4321;

/* ── Internal helpers ───────────────────────────────────────────────── */

/**
 * dns_build_query - Build a DNS query packet for a single record type
 * @buf:    output buffer (must be at least 512 bytes)
 * @name:   hostname to query
 * @qtype:  DNS record type (e.g., DNS_TYPE_A)
 * @txid:   transaction ID
 *
 * Returns: total packet length, or negative errno on failure.
 */
static int dns_build_query(uint8_t *buf, const char *name,
                           uint16_t qtype, uint16_t txid)
{
    struct dns_header *hdr;
    int pos;
    const char *p;
    const char *dot;
    int label_len;

    if (!buf || !name)
        return -EINVAL;

    /* Header */
    hdr = (struct dns_header *)buf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->id      = htons(txid);
    hdr->flags   = htons(DNS_QR_QUERY | DNS_OPCODE_STD | DNS_RD);
    hdr->qdcount = htons(1);

    pos = sizeof(struct dns_header);

    /* Encode name as sequence of length-prefixed labels */
    p = name;
    while (*p) {
        dot = p;
        while (*dot && *dot != '.')
            dot++;
        label_len = (int)(dot - p);
        if (label_len > 63)
            return -EINVAL;
        if (pos + 1 + label_len + 5 > 512)  /* room for label + QTYPE/QCLASS */
            return -ENOSPC;
        buf[pos++] = (uint8_t)label_len;
        memcpy(buf + pos, p, (size_t)label_len);
        pos += label_len;
        p = *dot ? dot + 1 : dot;
    }
    buf[pos++] = 0;  /* root label */

    /* QTYPE and QCLASS */
    buf[pos++] = (uint8_t)(qtype >> 8);
    buf[pos++] = (uint8_t)(qtype & 0xFF);
    buf[pos++] = 0;            /* CLASS high byte */
    buf[pos++] = DNS_CLASS_IN;

    return pos;
}

/**
 * dns_skip_name - Skip a DNS name (possibly compressed) without decoding
 *
 * Handles both regular label sequences and RFC 1035 compression pointers
 * (0xC0 prefix).  Returns the offset just past the name on success, or
 * negative errno if the data is malformed.
 */
static int dns_skip_name(const uint8_t *data, int data_len, int offset)
{
    int hops = 0;

    while (offset < data_len && hops < 128) {
        hops++;
        uint8_t label = data[offset];
        if (label == 0)
            return offset + 1;                      /* root label */
        if ((label & 0xC0) == 0xC0)
            return offset + 2;                      /* compression pointer */
        offset += 1 + (int)label;
        if (offset > data_len)
            return -EINVAL;
    }
    return -EINVAL;
}

/**
 * dns_resolver_parse_response - Parse DNS response for A record
 * @data:    raw DNS response data
 * @len:     length of response in bytes
 * @txid:    expected transaction ID
 * @out_ip:  receives IPv4 address in host byte order
 * @out_ttl: receives TTL value in seconds
 *
 * Returns: 0 on success, negative errno on failure:
 *   -EAGAIN   transaction ID mismatch (response belongs to another query)
 *   -ENOENT   NXDOMAIN or no A record in response
 *   -EIO      malformed response or server error
 */
static int dns_resolver_parse_response(const uint8_t *data, uint16_t len,
                                       uint16_t txid,
                                       uint32_t *out_ip, uint32_t *out_ttl)
{
    const struct dns_header *hdr;
    uint16_t flags;
    uint8_t rcode;
    uint16_t qdcount, ancount;
    int pos, ret;
    uint16_t a;

    if (!data || len < sizeof(struct dns_header) || !out_ip)
        return -EINVAL;

    hdr = (const struct dns_header *)data;

    /* Verify transaction ID */
    if (ntohs(hdr->id) != txid)
        return -EAGAIN;  /* not our response — caller may retry */

    /* Verify QR bit (must be a response) */
    flags = ntohs(hdr->flags);
    if (!(flags & DNS_QR_RESPONSE))
        return -EIO;

    /* Check RCODE */
    rcode = (uint8_t)(flags & 0x0F);
    if (rcode != DNS_RCODE_OK) {
        if (rcode == DNS_RCODE_NX)
            return -ENOENT;
        return -EIO;
    }

    qdcount = ntohs(hdr->qdcount);
    ancount = ntohs(hdr->ancount);

    if (ancount == 0)
        return -ENOENT;

    pos = sizeof(struct dns_header);

    /* Skip question section */
    for (uint16_t q = 0; q < qdcount; q++) {
        ret = dns_skip_name(data, (int)len, pos);
        if (ret < 0) return ret;
        pos = ret + 4;  /* skip QTYPE (2) + QCLASS (2) */
        if (pos > (int)len) return -EIO;
    }

    /* Parse answer section — look for the first A record */
    for (a = 0; a < ancount && pos + 12 <= (int)len; a++) {
        ret = dns_skip_name(data, (int)len, pos);
        if (ret < 0) return ret;
        pos = ret;

        if (pos + 10 > (int)len)
            return -EIO;

        uint16_t rtype = ((uint16_t)data[pos] << 8) | data[pos + 1];
        uint32_t ttl   = ((uint32_t)data[pos + 4] << 24) |
                         ((uint32_t)data[pos + 5] << 16) |
                         ((uint32_t)data[pos + 6] << 8)  |
                         data[pos + 7];
        uint16_t rdlen = ((uint16_t)data[pos + 8] << 8) | data[pos + 9];
        pos += 10;

        if (rtype == DNS_TYPE_A && rdlen == 4 && pos + 4 <= (int)len) {
            uint32_t ip = ((uint32_t)data[pos]     << 24) |
                          ((uint32_t)data[pos + 1] << 16) |
                          ((uint32_t)data[pos + 2] << 8)  |
                          data[pos + 3];
            *out_ip  = ip;
            if (out_ttl)
                *out_ttl = ttl;
            return 0;
        }

        pos += rdlen;
    }

    return -ENOENT;  /* no A record found */
}

/* ── AAAA record response parser ────────────────────────────────────── */

/**
 * dns_resolver_parse_response_aaaa - Parse DNS response for AAAA record
 * @data:    raw DNS response data
 * @len:     length of response in bytes
 * @txid:    expected transaction ID
 * @out_addr: receives IPv6 address (16 bytes, network byte order)
 * @out_ttl: receives TTL value in seconds
 *
 * Returns: 0 on success, negative errno on failure:
 *   -EAGAIN   transaction ID mismatch (response belongs to another query)
 *   -ENOENT   NXDOMAIN or no AAAA record in response
 *   -EIO      malformed response or server error
 */
static int dns_resolver_parse_response_aaaa(const uint8_t *data, uint16_t len,
                                             uint16_t txid,
                                             struct in6_addr *out_addr,
                                             uint32_t *out_ttl)
{
    const struct dns_header *hdr;
    uint16_t flags;
    uint8_t rcode;
    uint16_t qdcount, ancount;
    int pos, ret;
    uint16_t a;

    if (!data || len < sizeof(struct dns_header) || !out_addr)
        return -EINVAL;

    hdr = (const struct dns_header *)data;

    /* Verify transaction ID */
    if (ntohs(hdr->id) != txid)
        return -EAGAIN;

    /* Verify QR bit (must be a response) */
    flags = ntohs(hdr->flags);
    if (!(flags & DNS_QR_RESPONSE))
        return -EIO;

    /* Check RCODE */
    rcode = (uint8_t)(flags & 0x0F);
    if (rcode != DNS_RCODE_OK) {
        if (rcode == DNS_RCODE_NX)
            return -ENOENT;
        return -EIO;
    }

    qdcount = ntohs(hdr->qdcount);
    ancount = ntohs(hdr->ancount);

    if (ancount == 0)
        return -ENOENT;

    pos = sizeof(struct dns_header);

    /* Skip question section */
    for (uint16_t q = 0; q < qdcount; q++) {
        ret = dns_skip_name(data, (int)len, pos);
        if (ret < 0) return ret;
        pos = ret + 4;  /* skip QTYPE (2) + QCLASS (2) */
        if (pos > (int)len) return -EIO;
    }

    /* Parse answer section — look for the first AAAA record */
    for (a = 0; a < ancount && pos + 12 <= (int)len; a++) {
        ret = dns_skip_name(data, (int)len, pos);
        if (ret < 0) return ret;
        pos = ret;

        if (pos + 10 > (int)len)
            return -EIO;

        uint16_t rtype = ((uint16_t)data[pos] << 8) | data[pos + 1];
        uint32_t ttl   = ((uint32_t)data[pos + 4] << 24) |
                         ((uint32_t)data[pos + 5] << 16) |
                         ((uint32_t)data[pos + 6] << 8)  |
                         data[pos + 7];
        uint16_t rdlen = ((uint16_t)data[pos + 8] << 8) | data[pos + 9];
        pos += 10;

        if (rtype == DNS_TYPE_AAAA && rdlen == 16 && pos + 16 <= (int)len) {
            memcpy(out_addr->s6_addr, data + pos, 16);
            if (out_ttl)
                *out_ttl = ttl;
            return 0;
        }

        pos += rdlen;
    }

    return -ENOENT;  /* no AAAA record found */
}

/* ── CNAME resolution chain ─────────────────────────────────────────── */

/**
 * dns_name_to_string - Decode a compressed DNS name to a string
 * @data:    raw DNS message buffer
 * @len:     total message length
 * @offset:  byte offset where the name starts
 * @out:     output buffer for the decoded name (null-terminated)
 * @out_max: size of output buffer
 *
 * Decodes length-prefixed labels with RFC 1035 compression pointer
 * support (0xC0 + 14-bit offset).  Labels are written dot-separated
 * into @out (e.g., "www.example.com").
 *
 * Returns: 0 on success, negative errno on failure:
 *   -EIO   malformed name or buffer overrun
 *   -ENOSPC output buffer too small
 */
static int dns_name_to_string(const uint8_t *data, int len, int offset,
                               char *out, int out_max)
{
    int pos = offset;
    int out_pos = 0;
    int hops = 0;

    if (!out || out_max <= 0)
        return -EINVAL;

    while (hops < 128) {
        if (pos >= len)
            return -EIO;

        uint8_t label = data[pos];

        /* Root label — end of name */
        if (label == 0)
            break;

        /* Compression pointer (RFC 1035 §4.1.4) */
        if ((label & 0xC0) == 0xC0) {
            if (pos + 2 > len)
                return -EIO;
            int ptr = ((int)(label & 0x3F) << 8) | data[pos + 1];
            if (ptr >= len)
                return -EIO;
            pos = ptr;
            hops++;
            continue;
        }

        /* Regular label */
        int label_len = (int)label;
        if (label_len > 63 || label_len == 0)
            return -EIO;
        if (pos + 1 + label_len > len)
            return -EIO;
        pos++;

        /* Append label with dot separator */
        if (out_pos + label_len + 1 >= out_max)
            return -ENOSPC;
        memcpy(out + out_pos, data + pos, (size_t)label_len);
        out_pos += label_len;
        out[out_pos++] = '.';
        pos += label_len;
        hops++;
    }

    /* Remove trailing dot and null-terminate */
    if (out_pos > 0 && out[out_pos - 1] == '.')
        out[out_pos - 1] = '\0';
    else if (out_pos < out_max)
        out[out_pos] = '\0';

    return 0;
}

/**
 * dns_parse_response_a_or_cname - Parse DNS response for A or CNAME
 * @data:         raw DNS response data
 * @len:          response length in bytes
 * @txid:         expected transaction ID
 * @out_ip:       receives IPv4 address if A record found (host byte order)
 * @out_ttl:      receives TTL in seconds (may be NULL)
 * @cname_buf:    buffer for CNAME target name (if CNAME found, no A)
 * @cname_buf_sz: size of cname_buf
 *
 * Parses a DNS response looking for either an A record or a CNAME record
 * in the answer section.  Use this for CNAME chain following.
 *
 * Returns:
 *   0         A record found — *out_ip and *out_ttl are set
 *   1         CNAME found, no A record — cname_buf contains canonical name
 *   -EAGAIN   transaction ID mismatch
 *   -ENOENT   NXDOMAIN or no matching records in answer section
 *   -EIO      malformed response or server error
 */
static int dns_parse_response_a_or_cname(const uint8_t *data, uint16_t len,
                                          uint16_t txid,
                                          uint32_t *out_ip, uint32_t *out_ttl,
                                          char *cname_buf, int cname_buf_sz)
{
    const struct dns_header *hdr;
    uint16_t flags;
    uint8_t rcode;
    uint16_t qdcount, ancount;
    int pos, ret;
    uint16_t a;
    int found_cname = 0;
    uint32_t cname_ttl = 0;

    if (!data || len < sizeof(struct dns_header))
        return -EINVAL;

    hdr = (const struct dns_header *)data;

    /* Verify transaction ID */
    if (ntohs(hdr->id) != txid)
        return -EAGAIN;

    /* Verify QR bit (must be a response) */
    flags = ntohs(hdr->flags);
    if (!(flags & DNS_QR_RESPONSE))
        return -EIO;

    /* Check RCODE */
    rcode = (uint8_t)(flags & 0x0F);
    if (rcode != DNS_RCODE_OK) {
        if (rcode == DNS_RCODE_NX)
            return -ENOENT;
        return -EIO;
    }

    qdcount = ntohs(hdr->qdcount);
    ancount = ntohs(hdr->ancount);

    if (ancount == 0)
        return -ENOENT;

    pos = sizeof(struct dns_header);

    /* Skip question section */
    for (uint16_t q = 0; q < qdcount; q++) {
        ret = dns_skip_name(data, (int)len, pos);
        if (ret < 0) return ret;
        pos = ret + 4;  /* skip QTYPE (2) + QCLASS (2) */
        if (pos > (int)len) return -EIO;
    }

    /* Parse answer section */
    for (a = 0; a < ancount && pos + 12 <= (int)len; a++) {
        ret = dns_skip_name(data, (int)len, pos);
        if (ret < 0) return ret;
        pos = ret;

        if (pos + 10 > (int)len)
            return -EIO;

        uint16_t rtype = ((uint16_t)data[pos] << 8) | data[pos + 1];
        /* skip CLASS (2 bytes at pos+2, pos+3) */
        uint32_t ttl   = ((uint32_t)data[pos + 4] << 24) |
                         ((uint32_t)data[pos + 5] << 16) |
                         ((uint32_t)data[pos + 6] << 8)  |
                         data[pos + 7];
        uint16_t rdlen = ((uint16_t)data[pos + 8] << 8) | data[pos + 9];
        pos += 10;

        if (rtype == DNS_TYPE_A && rdlen == 4 && pos + 4 <= (int)len) {
            uint32_t ip = ((uint32_t)data[pos]     << 24) |
                          ((uint32_t)data[pos + 1] << 16) |
                          ((uint32_t)data[pos + 2] << 8)  |
                          data[pos + 3];
            *out_ip = ip;
            if (out_ttl)
                *out_ttl = ttl;
            return 0;  /* A record found — immediate success */
        }

        if (rtype == DNS_TYPE_CNAME && rdlen > 0 && pos + rdlen <= (int)len) {
            /* Decode the CNAME target (canonical name) */
            if (cname_buf && cname_buf_sz > 0) {
                ret = dns_name_to_string(data, (int)len, pos,
                                          cname_buf, cname_buf_sz);
                if (ret == 0) {
                    found_cname = 1;
                    cname_ttl   = ttl;
                }
            }
        }

        pos += rdlen;
    }

    /* No A record found — if we saw a CNAME, return it for chain following */
    if (found_cname) {
        if (out_ttl)
            *out_ttl = cname_ttl;
        return 1;
    }

    return -ENOENT;  /* no A or CNAME record in answer section */
}

/* ── Public API ─────────────────────────────────────────────────────── */

int dns_resolver_init(void)
{
    /* Seed the transaction ID counter with some entropy */
    dns_txid_counter = (uint16_t)(timer_get_ticks() ^ 0x5A5Au);
    return 0;
}

int dns_resolver_query_a(const char *hostname, uint32_t *out_ip, uint32_t *out_ttl)
{
    uint8_t pkt[512];
    uint8_t resp[512];
    uint32_t srv_ip;
    int ret, pkt_len;
    uint32_t ip = 0;
    uint32_t ttl = 0;
    uint16_t txid;
    int attempt;
    uint64_t deadline;
    uint32_t src_ip;
    uint16_t src_port;
    int rlen;
    int timeout;

    if (!hostname || !out_ip)
        return -EINVAL;

    *out_ip = 0;
    if (out_ttl)
        *out_ttl = 0;

    /* Get DNS server IP — use the global net_dns_server */
    srv_ip = net_dns_server;
    if (!srv_ip) {
        kprintf("[dns_resolver] no DNS server configured\n");
        return -EHOSTUNREACH;
    }

    /* Build the query packet */
    txid = (uint16_t)__sync_fetch_and_add(&dns_txid_counter, 1);
    ret = dns_build_query(pkt, hostname, DNS_TYPE_A, txid);
    if (ret < 0)
        return ret;
    pkt_len = ret;

    /* Start listening for DNS responses on our source port */
    ret = net_udp_listen(DNS_RESOLVER_SRC_PORT);
    if (ret < 0) {
        kprintf("[dns_resolver] failed to listen on port %d\n",
                DNS_RESOLVER_SRC_PORT);
        return ret;
    }

    /* Send query with retries */
    for (attempt = 0; attempt < DNS_RETRIES; attempt++) {
        /* Use a fresh transaction ID for each attempt */
        txid = (uint16_t)__sync_fetch_and_add(&dns_txid_counter, 1);
        pkt[0] = (uint8_t)(txid >> 8);
        pkt[1] = (uint8_t)(txid & 0xFF);

        /* Update the header ID in the packet */
        struct dns_header *hdr = (struct dns_header *)pkt;
        hdr->id = htons(txid);

        net_udp_send(srv_ip, DNS_RESOLVER_SRC_PORT, DNS_PORT, pkt, (uint16_t)pkt_len);

        /* Wait for response with timeout */
        deadline = timer_get_ticks() + DNS_TIMEOUT_TICKS;

        while (1) {
            uint64_t now = timer_get_ticks();
            if (now >= deadline)
                break;

            timeout = (int)(deadline - now);
            if (timeout <= 0)
                break;

            rlen = net_udp_recv(DNS_RESOLVER_SRC_PORT, resp, sizeof(resp),
                                &src_ip, &src_port, timeout);

            if (rlen > 0) {
                ret = dns_resolver_parse_response(resp, (uint16_t)rlen,
                                                  txid, &ip, &ttl);
                if (ret == 0) {
                    /* Success — A record found */
                    goto done;
                }
                if (ret != -EAGAIN) {
                    /* Fatal error — no point retrying */
                    goto done;
                }
                /* -EAGAIN means wrong transaction ID (stale or out-of-order
                 * response) — loop and keep waiting for ours */
            }
        }
    }

    /* All retries exhausted */
    ret = -ETIMEDOUT;

done:
    net_udp_unlisten(DNS_RESOLVER_SRC_PORT);

    if (ret == 0) {
        *out_ip = ip;
        if (out_ttl)
            *out_ttl = ttl;

        /* Cache the result with TTL from the DNS reply */
        dns_cache_store(hostname, ip, ttl);

        kprintf("[dns_resolver] %s -> %u.%u.%u.%u (ttl=%u)\n",
                hostname,
                (unsigned)((ip >> 24) & 0xFF),
                (unsigned)((ip >> 16) & 0xFF),
                (unsigned)((ip >>  8) & 0xFF),
                (unsigned)( ip        & 0xFF),
                (unsigned)ttl);
    }

    return ret;
}

/* ── CNAME chain following query ─────────────────────────────────────── */

int dns_resolver_query_a_follow_cname(const char *hostname,
                                       uint32_t *out_ip, uint32_t *out_ttl)
{
    char current_name[DNS_NAME_MAX];
    char cname_buf[DNS_NAME_MAX];
    uint8_t pkt[512];
    uint8_t resp[512];
    uint32_t srv_ip;
    uint32_t ttl = 0;
    uint32_t ip = 0;
    int chain_len;
    int ret;
    uint16_t txid;
    uint32_t src_ip;
    uint16_t src_port;
    int rlen;
    int timeout;
    int pkt_len;
    int a_found;
    uint64_t deadline;
    size_t name_len;

    if (!hostname || !out_ip)
        return -EINVAL;

    *out_ip = 0;
    if (out_ttl)
        *out_ttl = 0;

    /* Get DNS server IP */
    srv_ip = net_dns_server;
    if (!srv_ip) {
        kprintf("[dns_resolver] no DNS server configured\n");
        return -EHOSTUNREACH;
    }

    /* Start listening for DNS responses */
    ret = net_udp_listen(DNS_RESOLVER_SRC_PORT);
    if (ret < 0) {
        kprintf("[dns_resolver] failed to listen on port %d\n",
                DNS_RESOLVER_SRC_PORT);
        return ret;
    }

    /* Work with a copy of the hostname — we may modify it for CNAME follow */
    name_len = strlen(hostname);
    if (name_len >= sizeof(current_name))
        name_len = sizeof(current_name) - 1;
    memcpy(current_name, hostname, name_len);
    current_name[name_len] = '\0';

    a_found = 0;

    for (chain_len = 0; chain_len < DNS_CNAME_MAX_CHAIN; chain_len++) {
        /* Build query for A record of current name */
        txid = (uint16_t)__sync_fetch_and_add(&dns_txid_counter, 1);
        ret = dns_build_query(pkt, current_name, DNS_TYPE_A, txid);
        if (ret < 0)
            goto done;
        pkt_len = ret;

        /* Send query with retries */
        for (int attempt = 0; attempt < DNS_RETRIES; attempt++) {
            /* Use fresh TXID per attempt */
            txid = (uint16_t)__sync_fetch_and_add(&dns_txid_counter, 1);
            struct dns_header *hdr = (struct dns_header *)pkt;
            hdr->id = htons(txid);

            net_udp_send(srv_ip, DNS_RESOLVER_SRC_PORT, DNS_PORT,
                         pkt, (uint16_t)pkt_len);

            /* Wait for response with timeout */
            deadline = timer_get_ticks() + DNS_TIMEOUT_TICKS;

            while (1) {
                uint64_t now = timer_get_ticks();
                if (now >= deadline)
                    break;

                timeout = (int)(deadline - now);
                if (timeout <= 0)
                    break;

                rlen = net_udp_recv(DNS_RESOLVER_SRC_PORT, resp, sizeof(resp),
                                    &src_ip, &src_port, timeout);

                if (rlen > 0) {
                    ip = 0;
                    cname_buf[0] = '\0';
                    ret = dns_parse_response_a_or_cname(resp, (uint16_t)rlen,
                                                        txid, &ip, &ttl,
                                                        cname_buf,
                                                        (int)sizeof(cname_buf));
                    if (ret == 0) {
                        /* A record found */
                        a_found = 1;
                        goto chain_done;
                    }
                    if (ret == 1 && cname_buf[0] != '\0') {
                        /* CNAME found — follow the chain */
                        goto chain_done;
                    }
                    if (ret != -EAGAIN) {
                        /* Fatal error — no point retrying */
                        goto done;
                    }
                    /* -EAGAIN: keep waiting for our TXID */
                }
            }
        }

        /* All retries exhausted for this chain step */
        ret = -ETIMEDOUT;
        goto done;

chain_done:
        if (a_found) {
            ret = 0;
            goto done;
        }

        /* Follow CNAME to next name */
        if (cname_buf[0] == '\0') {
            ret = -ENOENT;
            goto done;
        }

        kprintf("[dns_resolver] CNAME: %s -> %s (chain step %d)\n",
                current_name, cname_buf, chain_len + 1);

        /* Update current name to CNAME target and re-query */
        name_len = strlen(cname_buf);
        if (name_len >= sizeof(current_name))
            name_len = sizeof(current_name) - 1;
        memcpy(current_name, cname_buf, name_len);
        current_name[name_len] = '\0';
    }

    /* Exceeded maximum chain depth */
    ret = -ELOOP;

done:
    net_udp_unlisten(DNS_RESOLVER_SRC_PORT);

    if (ret == 0) {
        *out_ip = ip;
        if (out_ttl)
            *out_ttl = ttl;

        /* Cache result under the original hostname */
        dns_cache_store(hostname, ip, ttl);

        kprintf("[dns_resolver] %s -> %u.%u.%u.%u (ttl=%u, "
                "cname_chain=%d)\n",
                hostname,
                (unsigned)((ip >> 24) & 0xFF),
                (unsigned)((ip >> 16) & 0xFF),
                (unsigned)((ip >>  8) & 0xFF),
                (unsigned)( ip        & 0xFF),
                (unsigned)ttl,
                chain_len);
    }

    return ret;
}

/* ── AAAA record query (IPv6 DNS) ──────────────────────────────────── */

int dns_resolver_query_aaaa(const char *hostname, struct in6_addr *out_addr,
                            uint32_t *out_ttl)
{
    uint8_t pkt[512];
    uint8_t resp[512];
    int ret, pkt_len;
    uint32_t ttl = 0;
    uint16_t txid;
    int attempt;
    uint64_t deadline;
    uint32_t src_ip;
    uint16_t src_port;
    int rlen;
    int timeout;

    if (!hostname || !out_addr)
        return -EINVAL;

    memset(out_addr, 0, sizeof(*out_addr));
    if (out_ttl)
        *out_ttl = 0;

    /* Check if an IPv6 DNS server is configured */
    if (ipv6_addr_is_unspecified(&net_ipv6_dns)) {
        kprintf("[dns_resolver] no IPv6 DNS server configured\n");
        return -EHOSTUNREACH;
    }

    /* Build the query packet for AAAA record */
    txid = (uint16_t)__sync_fetch_and_add(&dns_txid_counter, 1);
    ret = dns_build_query(pkt, hostname, DNS_TYPE_AAAA, txid);
    if (ret < 0)
        return ret;
    pkt_len = ret;

    /* Start listening for DNS responses on our source port */
    ret = net_udp_listen(DNS_RESOLVER_SRC_PORT);
    if (ret < 0) {
        kprintf("[dns_resolver] failed to listen on port %d\n",
                DNS_RESOLVER_SRC_PORT);
        return ret;
    }

    /* Send query with retries */
    for (attempt = 0; attempt < DNS_RETRIES; attempt++) {
        /* Use a fresh transaction ID for each attempt */
        txid = (uint16_t)__sync_fetch_and_add(&dns_txid_counter, 1);
        pkt[0] = (uint8_t)(txid >> 8);
        pkt[1] = (uint8_t)(txid & 0xFF);

        /* Update the header ID in the packet */
        struct dns_header *hdr = (struct dns_header *)pkt;
        hdr->id = htons(txid);

        /* Send via UDP over IPv6 to the DNS server */
        send_udp_ipv6(&net_ipv6_dns, DNS_RESOLVER_SRC_PORT, DNS_PORT,
                      pkt, (uint16_t)pkt_len);

        /* Wait for response with timeout */
        deadline = timer_get_ticks() + DNS_TIMEOUT_TICKS;

        while (1) {
            uint64_t now = timer_get_ticks();
            if (now >= deadline)
                break;

            timeout = (int)(deadline - now);
            if (timeout <= 0)
                break;

            rlen = net_udp_recv(DNS_RESOLVER_SRC_PORT, resp, sizeof(resp),
                                &src_ip, &src_port, timeout);

            if (rlen > 0) {
                ret = dns_resolver_parse_response_aaaa(resp, (uint16_t)rlen,
                                                       txid, out_addr, &ttl);
                if (ret == 0) {
                    /* Success — AAAA record found */
                    goto done;
                }
                if (ret != -EAGAIN) {
                    /* Fatal error — no point retrying */
                    goto done;
                }
                /* -EAGAIN means wrong transaction ID — keep waiting */
            }
        }
    }

    /* All retries exhausted */
    ret = -ETIMEDOUT;

done:
    net_udp_unlisten(DNS_RESOLVER_SRC_PORT);

    if (ret == 0) {
        if (out_ttl)
            *out_ttl = ttl;

        kprintf("[dns_resolver] AAAA %s -> %02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x (ttl=%u)\n",
                hostname,
                (unsigned)out_addr->s6_addr[0],
                (unsigned)out_addr->s6_addr[1],
                (unsigned)out_addr->s6_addr[2],
                (unsigned)out_addr->s6_addr[3],
                (unsigned)out_addr->s6_addr[4],
                (unsigned)out_addr->s6_addr[5],
                (unsigned)out_addr->s6_addr[6],
                (unsigned)out_addr->s6_addr[7],
                (unsigned)out_addr->s6_addr[8],
                (unsigned)out_addr->s6_addr[9],
                (unsigned)out_addr->s6_addr[10],
                (unsigned)out_addr->s6_addr[11],
                (unsigned)out_addr->s6_addr[12],
                (unsigned)out_addr->s6_addr[13],
                (unsigned)out_addr->s6_addr[14],
                (unsigned)out_addr->s6_addr[15],
                (unsigned)ttl);
    }

    return ret;
}

/* ── SRV record query (RFC 2782) ────────────────────────────────────── */

/**
 * dns_resolver_parse_response_srv - Parse DNS response for SRV records
 * @data:    raw DNS response data
 * @len:     length of response in bytes
 * @txid:    expected transaction ID
 * @records: array to receive parsed SRV records
 * @count:   on input: max records; on output: records parsed
 * @out_ttl: receives TTL from the first SRV record (may be NULL)
 *
 * Parses the answer section and extracts all SRV (type 33) records.
 * Each SRV record yields priority, weight, port, and target hostname.
 *
 * Returns: 0 on success, negative errno on failure:
 *   -EAGAIN   transaction ID mismatch
 *   -ENOENT   NXDOMAIN or no SRV record found
 *   -EIO      malformed response or server error
 *   -ENOSPC   more SRV records than *count allows
 */
static int dns_resolver_parse_response_srv(const uint8_t *data, uint16_t len,
                                            uint16_t txid,
                                            struct dns_srv_record *records,
                                            int *count, uint32_t *out_ttl)
{
    const struct dns_header *hdr;
    uint16_t flags;
    uint8_t rcode;
    uint16_t qdcount, ancount;
    int pos, ret;
    uint16_t a;
    int max_records;
    int found = 0;

    if (!data || len < sizeof(struct dns_header) || !records || !count)
        return -EINVAL;

    max_records = *count;
    *count = 0;

    hdr = (const struct dns_header *)data;

    /* Verify transaction ID */
    if (ntohs(hdr->id) != txid)
        return -EAGAIN;

    /* Verify QR bit (must be a response) */
    flags = ntohs(hdr->flags);
    if (!(flags & DNS_QR_RESPONSE))
        return -EIO;

    /* Check RCODE */
    rcode = (uint8_t)(flags & 0x0F);
    if (rcode != DNS_RCODE_OK) {
        if (rcode == DNS_RCODE_NX)
            return -ENOENT;
        return -EIO;
    }

    qdcount = ntohs(hdr->qdcount);
    ancount = ntohs(hdr->ancount);

    if (ancount == 0)
        return -ENOENT;

    pos = sizeof(struct dns_header);

    /* Skip question section */
    for (uint16_t q = 0; q < qdcount; q++) {
        ret = dns_skip_name(data, (int)len, pos);
        if (ret < 0) return ret;
        pos = ret + 4;  /* skip QTYPE (2) + QCLASS (2) */
        if (pos > (int)len) return -EIO;
    }

    /* Parse answer section — extract SRV records */
    for (a = 0; a < ancount && pos + 12 <= (int)len; a++) {
        ret = dns_skip_name(data, (int)len, pos);
        if (ret < 0) return ret;
        pos = ret;

        if (pos + 10 > (int)len)
            return -EIO;

        uint16_t rtype = ((uint16_t)data[pos] << 8) | data[pos + 1];
        uint32_t ttl   = ((uint32_t)data[pos + 4] << 24) |
                         ((uint32_t)data[pos + 5] << 16) |
                         ((uint32_t)data[pos + 6] << 8)  |
                         data[pos + 7];
        uint16_t rdlen = ((uint16_t)data[pos + 8] << 8) | data[pos + 9];
        pos += 10;

        if (rtype == DNS_TYPE_SRV && rdlen >= 7) {
            /* SRV RDATA: Priority(2) Weight(2) Port(2) Target(variable) */
            if (pos + 6 > (int)len)
                return -EIO;

            if (found >= max_records)
                return -ENOSPC;

            struct dns_srv_record *rec = &records[found];
            rec->priority = ((uint16_t)data[pos] << 8) | data[pos + 1];
            rec->weight   = ((uint16_t)data[pos + 2] << 8) | data[pos + 3];
            rec->port     = ((uint16_t)data[pos + 4] << 8) | data[pos + 5];

            /* Decode target name */
            ret = dns_name_to_string(data, (int)len, pos + 6,
                                      rec->target, (int)sizeof(rec->target));
            if (ret < 0)
                return ret;

            if (out_ttl && found == 0)
                *out_ttl = ttl;

            found++;
        }

        pos += rdlen;
    }

    *count = found;

    if (found == 0)
        return -ENOENT;

    return 0;
}

int dns_resolver_query_srv(const char *hostname, struct dns_srv_record *records,
                           int *count, uint32_t *out_ttl)
{
    uint8_t pkt[512];
    uint8_t resp[512];
    uint32_t srv_ip;
    int ret, pkt_len;
    uint32_t ttl = 0;
    uint16_t txid;
    int attempt;
    uint64_t deadline;
    uint32_t src_ip;
    uint16_t src_port;
    int rlen;
    int timeout;

    if (!hostname || !records || !count || *count <= 0)
        return -EINVAL;

    /* Get DNS server IP */
    srv_ip = net_dns_server;
    if (!srv_ip) {
        kprintf("[dns_resolver] no DNS server configured for SRV query\n");
        return -EHOSTUNREACH;
    }

    if (out_ttl)
        *out_ttl = 0;

    /* Build the SRV query packet */
    txid = (uint16_t)__sync_fetch_and_add(&dns_txid_counter, 1);
    ret = dns_build_query(pkt, hostname, DNS_TYPE_SRV, txid);
    if (ret < 0)
        return ret;
    pkt_len = ret;

    /* Start listening for DNS responses on our source port */
    ret = net_udp_listen(DNS_RESOLVER_SRC_PORT);
    if (ret < 0) {
        kprintf("[dns_resolver] failed to listen on port %d\n",
                DNS_RESOLVER_SRC_PORT);
        return ret;
    }

    /* Send query with retries */
    for (attempt = 0; attempt < DNS_RETRIES; attempt++) {
        /* Use a fresh transaction ID for each attempt */
        txid = (uint16_t)__sync_fetch_and_add(&dns_txid_counter, 1);
        struct dns_header *hdr = (struct dns_header *)pkt;
        hdr->id = htons(txid);

        net_udp_send(srv_ip, DNS_RESOLVER_SRC_PORT, DNS_PORT,
                     pkt, (uint16_t)pkt_len);

        /* Wait for response with timeout */
        deadline = timer_get_ticks() + DNS_TIMEOUT_TICKS;

        while (1) {
            uint64_t now = timer_get_ticks();
            if (now >= deadline)
                break;

            timeout = (int)(deadline - now);
            if (timeout <= 0)
                break;

            rlen = net_udp_recv(DNS_RESOLVER_SRC_PORT, resp, sizeof(resp),
                                &src_ip, &src_port, timeout);

            if (rlen > 0) {
                ret = dns_resolver_parse_response_srv(resp, (uint16_t)rlen,
                                                       txid, records,
                                                       count, &ttl);
                if (ret == 0) {
                    /* Success — SRV record(s) found */
                    goto done;
                }
                if (ret != -EAGAIN) {
                    /* Fatal error — no point retrying */
                    goto done;
                }
            }
        }
    }

    /* All retries exhausted */
    ret = -ETIMEDOUT;

done:
    net_udp_unlisten(DNS_RESOLVER_SRC_PORT);

    if (ret == 0) {
        if (out_ttl)
            *out_ttl = ttl;

        kprintf("[dns_resolver] SRV %s -> %d records (ttl=%u)\n",
                hostname, *count, (unsigned)ttl);
    }

    return ret;
}
