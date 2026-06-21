/*
 * dns_server.c — Authoritative DNS server (RFC 1035)
 *
 * Listens on UDP 53, parses DNS queries for A, AAAA, CNAME, MX records
 * from a zone file (/etc/dns_zones.txt).  Unresolved queries are forwarded
 * to the upstream DNS resolver.
 */

#define KERNEL_INTERNAL
#include "dns_server.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "string_ext.h"
#include "stdlib.h"
#include "printf.h"
#include "timer.h"
#include "vfs.h"

/* ── Local zone storage ────────────────────────────────────────────── */

static struct dns_rr zone_records[DNS_MAX_RECORDS];
static int zone_count = 0;
static int server_running = 0;

/* Forward resolver: upstream DNS */
static uint32_t upstream_dns = 0;

/* ── DNS wire format helpers ───────────────────────────────────────── */

/* Encode a DNS name into wire format (label sequence) */
static int dns_encode_name(char *dst, int dst_size, const char *name) {
    int pos = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        int label_len = dot ? (int)(dot - name) : (int)strlen(name);
        if (label_len > 63) return -1;
        if (pos + 1 + label_len > dst_size) return -1;
        dst[pos++] = (uint8_t)label_len;
        memcpy(dst + pos, name, label_len);
        pos += label_len;
        name = dot ? dot + 1 : name + label_len;
    }
    if (pos + 1 > dst_size) return -1;
    dst[pos++] = 0;  /* root label */
    return pos;
}

/* Decode a DNS name from wire format (labels may be compressed) */
static int dns_decode_name(const uint8_t *data, int data_len, int offset,
                            char *name, int name_size) {
    int pos = 0;
    int jumped = 0;
    int start_offset = offset;

    while (offset < data_len) {
        uint8_t len = data[offset];
        if (len == 0) {
            offset++;
            break;
        }
        if (len & 0xC0) {
            /* Pointer: upper 2 bits 11 indicate compression */
            if (offset + 1 >= data_len) return -1;
            int ptr = ((len & 0x3F) << 8) | data[offset + 1];
            offset += 2;
            if (!jumped) {
                start_offset = offset;
                jumped = 1;
            }
            offset = ptr;
            continue;
        }
        offset++;
        if (offset + len > data_len) return -1;
        if (pos + len + 1 > name_size - 1) return -1;
        if (pos > 0) name[pos++] = '.';
        memcpy(name + pos, data + offset, len);
        pos += len;
        offset += len;
    }
    name[pos] = '\0';
    return jumped ? start_offset : offset;
}

/* ── Zone file parser ──────────────────────────────────────────────── */

/* sscanf replacement for kernel */
static int parse_ip(const char *s, uint32_t *ip) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    char buf[32];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok = buf;
    int parts = 0;
    char *dot;
    while ((dot = strchr(tok, '.')) != NULL && parts < 4) {
        *dot = '\0';
        unsigned long v = strtoul(tok, NULL, 10);
        if (parts == 0) a = (unsigned int)v;
        else if (parts == 1) b = (unsigned int)v;
        else if (parts == 2) c = (unsigned int)v;
        tok = dot + 1;
        parts++;
    }
    if (parts == 3 && *tok) {
        d = (unsigned int)strtoul(tok, NULL, 10);
        parts++;
    }
    if (parts == 4) {
        *ip = htonl((a << 24) | (b << 16) | (c << 8) | d);
        return 1;
    }
    return 0;
}

/* Simple tokenizer */
static char *next_token(char **pos) {
    while (**pos == ' ' || **pos == '\t') (*pos)++;
    if (**pos == '\0') return NULL;
    char *start = *pos;
    while (**pos && **pos != ' ' && **pos != '\t' && **pos != '\n') (*pos)++;
    if (**pos) {
        **pos = '\0';
        (*pos)++;
    }
    return start;
}

static int parse_zone_file(const char *path) {
    /* Try to open and read via vfs */
    uint32_t file_size = 0;
    char buf[DNS_MAX_ZONE_SIZE];
    int rc = vfs_read(path, buf, sizeof(buf) - 1, &file_size);
    if (rc < 0 || file_size == 0) {
        kprintf("[dns_server] No zone file at %s (using defaults)\n", path);
        return -1;
    }
    buf[file_size] = '\0';

    char *p = buf;
    char origin[DNS_NAME_MAX_LEN] = "";
    int line_num = 0;

    while (p && *p) {
        char *line = p;
        /* Find next newline */
        while (*p && *p != '\n') p++;
        if (*p == '\n') {
            *p = '\0';
            p++;
        }
        line_num++;

        /* Skip comments and empty lines */
        while (*line == ' ' || *line == '\t') line++;
        if (*line == ';' || *line == '#' || *line == '\0')
            continue;

        /* Check for $ORIGIN */
        if (strncmp(line, "$ORIGIN", 7) == 0) {
            char *val = line + 7;
            while (*val == ' ' || *val == '\t') val++;
            snprintf(origin, sizeof(origin), "%s", val);
            continue;
        }

        if (zone_count >= DNS_MAX_RECORDS) break;

        char *tok = line;
        char *name = next_token(&tok);
        if (!name) continue;
        char *type_str = next_token(&tok);
        if (!type_str) continue;
        char *rdata = next_token(&tok);
        if (!rdata) continue;

        struct dns_rr *rr = &zone_records[zone_count];

        /* Fully qualify name if needed */
        if (name[strlen(name) - 1] != '.' && origin[0]) {
            snprintf(rr->name, sizeof(rr->name), "%s.%s", name, origin);
        } else {
            snprintf(rr->name, sizeof(rr->name), "%s", name);
        }

        /* Normalize: remove trailing dot if present */
        int nl = strlen(rr->name);
        if (nl > 0 && rr->name[nl - 1] == '.')
            rr->name[nl - 1] = '\0';

        if (strcmp(type_str, "A") == 0 || strcmp(type_str, "a") == 0) {
            rr->type = DNS_TYPE_A;
            rr->rr_class = DNS_CLASS_IN;
            rr->ttl = 3600;
            if (parse_ip(rdata, &rr->data.a_record)) {
                zone_count++;
            }
        } else if (strcmp(type_str, "CNAME") == 0 || strcmp(type_str, "cname") == 0) {
            rr->type = DNS_TYPE_CNAME;
            rr->rr_class = DNS_CLASS_IN;
            rr->ttl = 3600;
            snprintf(rr->data.cname, sizeof(rr->data.cname), "%s", rdata);
            zone_count++;
        } else if (strcmp(type_str, "MX") == 0 || strcmp(type_str, "mx") == 0) {
            rr->type = DNS_TYPE_MX;
            rr->rr_class = DNS_CLASS_IN;
            rr->ttl = 3600;
            /* MX: preference hostname */
            char *pref_str = rdata;
            char *exch_str = rdata;
            while (*exch_str && *exch_str != ' ') exch_str++;
            if (*exch_str) {
                *exch_str = '\0';
                exch_str++;
                while (*exch_str == ' ') exch_str++;
            }
            rr->data.mx.preference = (uint16_t)atoi(pref_str);
            snprintf(rr->data.mx.exchange, sizeof(rr->data.mx.exchange), "%s", exch_str);
            zone_count++;
        } else if (strcmp(type_str, "SOA") == 0 || strcmp(type_str, "soa") == 0) {
            rr->type = DNS_TYPE_SOA;
            rr->rr_class = DNS_CLASS_IN;
            rr->ttl = 3600;
            /* SOA: mname rname serial refresh retry expire minimum */
            char *mname = next_token(&tok);
            char *rname = next_token(&tok);
            if (mname && rname) {
                snprintf(rr->data.soa.mname, sizeof(rr->data.soa.mname), "%s", mname);
                snprintf(rr->data.soa.rname, sizeof(rr->data.soa.rname), "%s", rname);
                rr->data.soa.serial = (uint32_t)atoi(next_token(&tok) ?: "1");
                rr->data.soa.refresh = (uint32_t)atoi(next_token(&tok) ?: "3600");
                rr->data.soa.retry = (uint32_t)atoi(next_token(&tok) ?: "900");
                rr->data.soa.expire = (uint32_t)atoi(next_token(&tok) ?: "86400");
                rr->data.soa.minimum = (uint32_t)atoi(next_token(&tok) ?: "3600");
                zone_count++;
            }
        }
    }

    kprintf("[dns_server] Loaded %d zone records from %s\n", zone_count, path);
    return zone_count;
}

/* ── Query handler ─────────────────────────────────────────────────── */

/* Build a DNS response with a single answer RR */
static int build_dns_response(const uint8_t *query, int query_len,
                               uint8_t *resp, int resp_size,
                               const struct dns_rr *answer) {
    if (query_len < 12) return -1;

    /* Copy header */
    memcpy(resp, query, 12);

    /* Set QR bit (response) and clear RCODE */
    resp[2] |= 0x80;  /* QR=1 */
    resp[3] &= 0x0F;  /* clear RCODE */

    /* QDCOUNT stays as was */
    uint16_t qdcount = (query[4] << 8) | query[5];
    uint16_t ancount = (answer != NULL) ? 1 : 0;

    /* Set ANCOUNT */
    resp[6] = (ancount >> 8) & 0xFF;
    resp[7] = ancount & 0xFF;

    /* NSCOUNT, ARCOUNT = 0 */
    resp[8] = resp[9] = resp[10] = resp[11] = 0;

    int pos = 12;

    /* Copy question section */
    int qpos = 12;
    while (qpos < query_len) {
        uint8_t len = query[qpos];
        if (len == 0) {
            qpos += 4;  /* root label + QTYPE + QCLASS */
            break;
        }
        if (len & 0xC0) {
            qpos += 2;
        } else {
            qpos += 1 + len;
        }
    }
    int question_len = qpos - 12;
    memcpy(resp + pos, query + 12, question_len);
    pos += question_len;

    /* Add answer if we have one */
    if (answer) {
        /* Encode owner name (as pointer to question) */
        if (pos + 2 > resp_size) return -1;
        resp[pos++] = 0xC0;  /* pointer */
        resp[pos++] = 0x0C;  /* points to start of question section in DNS header */

        /* TYPE */
        if (pos + 2 > resp_size) return -1;
        resp[pos++] = (answer->type >> 8) & 0xFF;
        resp[pos++] = answer->type & 0xFF;

        /* CLASS */
        if (pos + 2 > resp_size) return -1;
        resp[pos++] = (answer->rr_class >> 8) & 0xFF;
        resp[pos++] = answer->rr_class & 0xFF;

        /* TTL */
        if (pos + 4 > resp_size) return -1;
        uint32_t ttl = answer->ttl;
        resp[pos++] = (ttl >> 24) & 0xFF;
        resp[pos++] = (ttl >> 16) & 0xFF;
        resp[pos++] = (ttl >> 8) & 0xFF;
        resp[pos++] = ttl & 0xFF;

        /* RDLENGTH + RDATA */
        if (answer->type == DNS_TYPE_A) {
            if (pos + 6 > resp_size) return -1;
            resp[pos++] = 0;  /* RDLENGTH high byte */
            resp[pos++] = 4;  /* RDLENGTH low byte */
            uint32_t ip = answer->data.a_record;
            resp[pos++] = (ip >> 24) & 0xFF;
            resp[pos++] = (ip >> 16) & 0xFF;
            resp[pos++] = (ip >> 8) & 0xFF;
            resp[pos++] = ip & 0xFF;
        } else if (answer->type == DNS_TYPE_CNAME) {
            char encoded[DNS_NAME_MAX_LEN * 2];
            int elen = dns_encode_name(encoded, sizeof(encoded), answer->data.cname);
            if (elen < 0) return -1;
            if (pos + 2 + elen > resp_size) return -1;
            resp[pos++] = (elen >> 8) & 0xFF;
            resp[pos++] = elen & 0xFF;
            memcpy(resp + pos, encoded, elen);
            pos += elen;
        } else if (answer->type == DNS_TYPE_MX) {
            char encoded[DNS_NAME_MAX_LEN * 2];
            int elen = dns_encode_name(encoded, sizeof(encoded), answer->data.mx.exchange);
            if (elen < 0) return -1;
            if (pos + 2 + 2 + elen > resp_size) return -1;
            uint16_t rdlen = 2 + elen;
            resp[pos++] = (rdlen >> 8) & 0xFF;
            resp[pos++] = rdlen & 0xFF;
            resp[pos++] = (answer->data.mx.preference >> 8) & 0xFF;
            resp[pos++] = answer->data.mx.preference & 0xFF;
            memcpy(resp + pos, encoded, elen);
            pos += elen;
        } else if (answer->type == DNS_TYPE_SOA) {
            char encoded_m[DNS_NAME_MAX_LEN * 2];
            char encoded_r[DNS_NAME_MAX_LEN * 2];
            int em = dns_encode_name(encoded_m, sizeof(encoded_m), answer->data.soa.mname);
            int er = dns_encode_name(encoded_r, sizeof(encoded_r), answer->data.soa.rname);
            if (em < 0 || er < 0) return -1;
            int soa_len = em + er + 20;  /* 5 uint32 after names */
            if (pos + 2 + soa_len > resp_size) return -1;
            resp[pos++] = (soa_len >> 8) & 0xFF;
            resp[pos++] = soa_len & 0xFF;
            memcpy(resp + pos, encoded_m, em); pos += em;
            memcpy(resp + pos, encoded_r, er); pos += er;
            /* serial, refresh, retry, expire, minimum */
            uint32_t soa_vals[] = {
                htonl(answer->data.soa.serial),
                htonl(answer->data.soa.refresh),
                htonl(answer->data.soa.retry),
                htonl(answer->data.soa.expire),
                htonl(answer->data.soa.minimum)
            };
            memcpy(resp + pos, soa_vals, sizeof(soa_vals));
            pos += sizeof(soa_vals);
        } else {
            /* Unsupported type — no answer */
            resp[6] = resp[7] = 0;
        }
    }

    return pos;
}

/* Look up a query name in the zone records */
static struct dns_rr *dns_zone_lookup(const char *qname, uint16_t qtype) {
    for (int i = 0; i < zone_count; i++) {
        if (zone_records[i].type == qtype &&
            strcasecmp(zone_records[i].name, qname) == 0) {
            return &zone_records[i];
        }
    }
    return NULL;
}

/* Handle incoming DNS queries */
static void handle_dns_query(uint32_t src_ip, uint16_t src_port,
                              const uint8_t *data, uint16_t len) {
    (void)src_ip;

    if (len < 12) return;

    /* Check QR bit — must be a query (QR=0) */
    if (data[2] & 0x80) return;

    uint16_t qdcount = (data[4] << 8) | data[5];
    if (qdcount == 0) return;

    /* Decode the question name */
    char qname[DNS_NAME_MAX_LEN];
    if (dns_decode_name(data, len, 12, qname, sizeof(qname)) < 0) return;

    /* Read QTYPE and QCLASS */
    int offset = 12;
    while (offset < len) {
        uint8_t l = data[offset];
        if (l == 0) { offset++; break; }
        if (l & 0xC0) { offset += 2; break; }
        offset += 1 + l;
    }
    if (offset + 4 > len) return;
    uint16_t qtype = (data[offset] << 8) | data[offset + 1];

    kprintf("[dns_server] Query: %s type=%u\n", qname, qtype);

    uint8_t resp[512];
    int resp_len;

    /* Try local zone lookup */
    struct dns_rr *answer = dns_zone_lookup(qname, qtype);

    if (answer) {
        resp_len = build_dns_response(data, len, resp, sizeof(resp), answer);
        if (resp_len > 0) {
            net_udp_send(src_ip, DNS_SERVER_PORT, src_port, resp, resp_len);
        }
        return;
    }

    /* For A queries with no local match, try forwarding */
    if (qtype == DNS_TYPE_A && upstream_dns) {
        uint32_t resolved = net_dns_resolve(qname);
        if (resolved) {
            struct dns_rr rr;
            memset(&rr, 0, sizeof(rr));
            snprintf(rr.name, sizeof(rr.name), "%s", qname);
            rr.type = DNS_TYPE_A;
            rr.rr_class = DNS_CLASS_IN;
            rr.ttl = 60;
            rr.data.a_record = htonl(resolved);
            resp_len = build_dns_response(data, len, resp, sizeof(resp), &rr);
            if (resp_len > 0) {
                net_udp_send(src_ip, DNS_SERVER_PORT, src_port, resp, resp_len);
            }
            return;
        }
    }

    /* NXDOMAIN */
    resp_len = build_dns_response(data, len, resp, sizeof(resp), NULL);
    if (resp_len > 0) {
        resp[3] |= 3;  /* NXDOMAIN */
        net_udp_send(src_ip, DNS_SERVER_PORT, src_port, resp, resp_len);
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

int dns_server_init(void) {
    if (server_running) return 0;

    /* Clear zone */
    memset(zone_records, 0, sizeof(zone_records));
    zone_count = 0;

    /* Try to parse zone file */
    parse_zone_file("/etc/dns_zones.txt");

    /* Get upstream DNS from network config */
    upstream_dns = net_get_dns();
    if (!upstream_dns) {
        upstream_dns = net_resolv_conf_read_first();
    }

    /* Bind UDP handler on port 53 */
    net_udp_bind(DNS_SERVER_PORT, handle_dns_query);

    server_running = 1;
    kprintf("[dns_server] Listening on UDP 53 (upstream: %u.%u.%u.%u)\n",
            (upstream_dns >> 24) & 0xFF, (upstream_dns >> 16) & 0xFF,
            (upstream_dns >> 8) & 0xFF, upstream_dns & 0xFF);

    return 0;
}

void dns_server_stop(void) {
    if (!server_running) return;
    net_udp_bind(DNS_SERVER_PORT, NULL);
    server_running = 0;
    kprintf("[dns_server] Stopped\n");
}
#include "module.h"
module_init(dns_server_init);

/* ── Stub: dns_server_start ─────────────────────────────── */
int dns_server_start(int port)
{
    (void)port;
    kprintf("[dns] dns_server_start: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: dns_server_handle_query ─────────────────────────────── */
int dns_server_handle_query(const void *query, size_t len, void *resp, size_t *rlen)
{
    (void)query;
    (void)len;
    (void)resp;
    (void)rlen;
    kprintf("[dns] dns_server_handle_query: not yet implemented\n");
    return -ENOSYS;
}
