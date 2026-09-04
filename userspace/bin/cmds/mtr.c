/* mtr.c — per-hop ICMP traceroute probes (My Traceroute).
 *
 * Sends an ICMP echo probe with an increasing IP hop-limit (TTL) for each
 * hop via net_trace(ip, ttl). The first reply to a TTL-limited probe is the
 * router that consumed the TTL (ICMP Time-Exceeded); once the probe reaches
 * the destination the echo reply source is the target itself. Prints each
 * hop's address, stopping when the destination answers.
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define IP_FMT(ip) ((ip) >> 24) & 0xFF, ((ip) >> 16) & 0xFF, ((ip) >> 8) & 0xFF, (ip) & 0xFF
#define DEFAULT_MAX_HOPS 30

static unsigned int parse_ip(const char *s) {
    unsigned int ip = 0;
    int shift = 24;
    while (*s) {
        unsigned int val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (unsigned int)(*s - '0');
            s++;
        }
        ip |= (val << shift);
        shift -= 8;
        if (*s == '.') s++;
        else break;
    }
    return ip;
}

static int is_hostname(const char *s) {
    while (*s) {
        if (!((*s >= '0' && *s <= '9') || *s == '.')) return 1;
        s++;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: mtr <host> [max-hops]\n");
        return 1;
    }

    unsigned int ip;
    if (is_hostname(argv[1])) {
        int dns_ret = net_dns(argv[1]);
        if (dns_ret < 0) {
            printf("mtr: could not resolve %s\n", argv[1]);
            return 1;
        }
        ip = (unsigned int)dns_ret;
    } else {
        ip = parse_ip(argv[1]);
    }

    int max_hops = DEFAULT_MAX_HOPS;
    if (argc > 2) {
        max_hops = atoi(argv[2]);
        if (max_hops <= 0) max_hops = DEFAULT_MAX_HOPS;
        if (max_hops > 64) max_hops = 64;
    }

    printf("mtr to %d.%d.%d.%d, %d hops max:\n", IP_FMT(ip), max_hops);

    int reached = 0;
    for (int ttl = 1; ttl <= max_hops && !reached; ttl++) {
        int hop = net_trace(ip, ttl);
        printf("%2d  ", ttl);
        if (hop == -1) {
            printf("*\n"); /* no reply for this TTL */
            continue;
        }
        unsigned int hop_ip = (unsigned int)hop;
        printf("%d.%d.%d.%d\n", IP_FMT(hop_ip));
        if (hop_ip == ip)
            reached = 1;
    }

    return reached ? 0 : 1;
}