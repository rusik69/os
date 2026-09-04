/* mtr.c — per-hop ICMP traceroute probes with RTT stats (My Traceroute).
 *
 * Sends ICMP echo probes with an increasing IP hop-limit (TTL) for each hop
 * via net_trace(ip, ttl). The first reply to a TTL-limited probe is the
 * router that consumed the TTL (ICMP Time-Exceeded); once the probe reaches
 * the destination the echo reply source is the target itself. Each hop is
 * probed PROBES_PER_HOP times and per-hop min/avg/max round-trip times are
 * reported alongside the hop address.
 */
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

#define IP_FMT(ip) ((ip) >> 24) & 0xFF, ((ip) >> 16) & 0xFF, ((ip) >> 8) & 0xFF, (ip) & 0xFF
#define DEFAULT_MAX_HOPS 30
#define PROBES_PER_HOP 3

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
        if (*s == '.')
            s++;
        else
            break;
    }
    return ip;
}

static int is_hostname(const char *s) {
    while (*s) {
        if (!((*s >= '0' && *s <= '9') || *s == '.'))
            return 1;
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
        if (max_hops <= 0)
            max_hops = DEFAULT_MAX_HOPS;
        if (max_hops > 64)
            max_hops = 64;
    }

    printf("mtr: %d.%d.%d.%d (%d hops max, %d probes/hop)\n", IP_FMT(ip), max_hops, PROBES_PER_HOP);
    printf("%3s  %-15s  %5s %5s %5s %5s %4s\n", "Hop", "Host", "Loss%", "Min", "Avg", "Max", "Snt");

    int reached = 0;
    for (int ttl = 1; ttl <= max_hops && !reached; ttl++) {
        unsigned long long rtt[PROBES_PER_HOP];
        int ok = 0;
        unsigned int hop_ip = 0;

        for (int p = 0; p < PROBES_PER_HOP; p++) {
            struct timespec ts1, ts2;
            int have_time = 0;
            if (clock_gettime(0, &ts1) == 0)
                have_time = 1;

            int hop = net_trace(ip, ttl);

            unsigned long long r = 0;
            if (have_time && clock_gettime(0, &ts2) == 0) {
                r = (ts2.tv_sec - ts1.tv_sec) * 1000ULL + (ts2.tv_nsec - ts1.tv_nsec) / 1000000ULL;
            }

            if (hop >= 0) {
                rtt[ok++] = r;
                hop_ip = (unsigned int)hop;
            }
        }

        if (ok == 0) {
            printf("%3d  %-15s  %5d %6s %6s %6s %4d\n", ttl, "*", 100, "-", "-", "-",
                   PROBES_PER_HOP);
            continue;
        }

        unsigned long long mn = rtt[0], mx = rtt[0], sum = 0;
        for (int i = 0; i < ok; i++) {
            if (rtt[i] < mn)
                mn = rtt[i];
            if (rtt[i] > mx)
                mx = rtt[i];
            sum += rtt[i];
        }
        int loss = (PROBES_PER_HOP - ok) * 100 / PROBES_PER_HOP;
        unsigned long long avg = ok > 0 ? sum / (unsigned long long)ok : 0;

        printf("%3d  %d.%d.%d.%d  %5d %5llu %5llu %5llu %4d\n", ttl, IP_FMT(hop_ip), loss, mn, avg,
               mx, ok);

        if (hop_ip == ip)
            reached = 1;
    }

    return reached ? 0 : 1;
}