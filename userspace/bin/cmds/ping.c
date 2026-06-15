/* ping.c — ICMP ping: resolve hostname, ping with count, show stats */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define IP_FMT(ip) ((ip) >> 24) & 0xFF, ((ip) >> 16) & 0xFF, ((ip) >> 8) & 0xFF, (ip) & 0xFF

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
        printf("Usage: ping <host> [count]\n");
        return 1;
    }

    unsigned int ip;
    if (is_hostname(argv[1])) {
        int dns_ret = net_dns(argv[1]);
        if (dns_ret < 0) {
            printf("ping: could not resolve %s\n", argv[1]);
            return 1;
        }
        ip = (unsigned int)dns_ret;
    } else {
        ip = parse_ip(argv[1]);
    }

    int count = 4;
    if (argc > 2) {
        count = atoi(argv[2]);
        if (count <= 0) count = 1;
    }

    int sent = 0, received = 0;
    printf("Pinging %d.%d.%d.%d with 32 bytes of data:\n", IP_FMT(ip));

    for (int i = 0; i < count; i++) {
        sent++;
        struct timespec ts1, ts2;
        int have_time = 0;
        if (clock_gettime(0, &ts1) == 0) have_time = 1;

        int result = net_ping(ip);

        unsigned long long rtt = 0;
        if (have_time && clock_gettime(0, &ts2) == 0) {
            rtt = (ts2.tv_sec - ts1.tv_sec) * 1000ULL
                + (ts2.tv_nsec - ts1.tv_nsec) / 1000000ULL;
        }

        if (result == 0) {
            received++;
            printf("Reply from %d.%d.%d.%d: time=%llums\n", IP_FMT(ip), rtt);
        } else {
            printf("Request timed out.\n");
        }
    }

    int lost = sent - received;
    int pct = sent > 0 ? (lost * 100 / sent) : 0;
    printf("\nPing statistics for %d.%d.%d.%d:\n", IP_FMT(ip));
    printf("    Packets: Sent = %d, Received = %d, Lost = %d (%d%% loss)\n",
           sent, received, lost, pct);

    return (received > 0) ? 0 : 1;
}
