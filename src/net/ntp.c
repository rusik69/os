/*
 * ntp.c — NTP client implementation (RFC 5905)
 *
 * Sends NTP mode 3 (client) packets via UDP port 123, parses the
 * server response and computes clock offset for system time setting.
 */

#define KERNEL_INTERNAL
#include "ntp.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "time.h"

/* Timeout for NTP response (seconds) */
#define NTP_TIMEOUT_SEC 5

/* NTP short format: 16-bit seconds + 16-bit fraction */
static inline int32_t ntp_short_to_ms(int32_t ntp_short) {
    /* ntp_short is seconds in upper 16 bits, fraction in lower 16 bits */
    int32_t sec = ntp_short >> 16;
    /* fraction * 1000 / 65536 */
    uint32_t frac_ms = ((uint32_t)(ntp_short & 0xFFFF) * 1000) >> 16;
    return sec * 1000 + (int32_t)frac_ms;
}

/* Convert NTP 64-bit fixed point to microseconds fraction */
static inline uint32_t ntp_frac_to_usec(uint32_t fraction) {
    /* fraction is 2^-32 seconds, convert to microseconds */
    /* fraction * 1000000 / 2^32 */
    return (uint32_t)(((uint64_t)fraction * 1000000) >> 32);
}

/* NTP state for synchronous request */
static volatile int ntp_reply_received = 0;
static volatile uint64_t ntp_reply_sec = 0;
static volatile uint32_t ntp_reply_usec = 0;
static uint16_t ntp_txid = 0;

static void handle_ntp_reply(uint32_t src_ip, uint16_t src_port,
                              const uint8_t *data, uint16_t len) {
    (void)src_ip;
    (void)src_port;

    if (len < sizeof(struct ntp_packet)) return;

    const struct ntp_packet *pkt = (const struct ntp_packet *)data;

    uint8_t mode = pkt->li_vn_mode & 0x07;
    uint8_t stratum = pkt->stratum;

    /* Must be server mode (4) and valid stratum */
    if (mode != NTP_MODE_SERVER) return;
    if (stratum == 0 || stratum > 15) return;  /* kiss-o'-death or invalid */

    /* Get transmit timestamp from server (t3) */
    uint32_t t3_sec = ntohl(pkt->tx_ts.seconds);
    uint32_t t3_frac = ntohl(pkt->tx_ts.fraction);

    /* Get origin timestamp (should match our client timestamp = t1) */
    /* We don't validate this strictly in the simple client */

    /* Compute time from server transmit timestamps alone (simple SNTP) */
    uint64_t server_sec = ntp_to_unix_sec(t3_sec);
    uint32_t server_usec = ntp_frac_to_usec(t3_frac);

    ntp_reply_sec = server_sec;
    ntp_reply_usec = server_usec;
    ntp_reply_received = 1;
}

int ntp_request(uint32_t server_ip, uint64_t *out_sec, uint32_t *out_usec) {
    if (!out_sec || !out_usec) return -1;

    ntp_reply_received = 0;

    /* Bind UDP handler on ephemeral port */
    uint16_t sport = 0xCDEF;  /* fixed ephemeral port for NTP */

    net_udp_bind(sport, handle_ntp_reply);

    /* Build NTP packet */
    struct ntp_packet pkt;
    memset(&pkt, 0, sizeof(pkt));

    /* LI=0, VN=4, Mode=3 */
    pkt.li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;
    pkt.stratum = 0;
    pkt.poll = 4;   /* 2^4 = 16 seconds poll interval suggestion */
    pkt.precision = 0;  /* let precision be 0 for simplicity */
    pkt.root_delay = 0;
    pkt.root_dispersion = 0;
    pkt.reference_id = 0;

    /* Set origin timestamp (t1) — current system time */
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    pkt.orig_ts.seconds = htonl(unix_to_ntp_sec(now.tv_sec));
    pkt.orig_ts.fraction = htonl((uint32_t)(((uint64_t)now.tv_nsec << 32) / 1000000000));

    /* Set transmit timestamp */
    pkt.tx_ts.seconds = pkt.orig_ts.seconds;
    pkt.tx_ts.fraction = pkt.orig_ts.fraction;

    /* Reference and receive timestamps are 0 for client */
    memset(&pkt.ref_ts, 0, sizeof(pkt.ref_ts));
    memset(&pkt.rx_ts, 0, sizeof(pkt.rx_ts));

    /* Send NTP query */
    net_udp_send(server_ip, sport, NTP_PORT, &pkt, sizeof(pkt));

    /* Wait for reply with timeout */
    uint64_t deadline = timer_get_ticks() + (NTP_TIMEOUT_SEC * 100);

    while (!ntp_reply_received && timer_get_ticks() < deadline) {
        net_poll();
    }

    /* Unbind handler */
    net_udp_bind(sport, NULL);

    if (!ntp_reply_received) {
        return -1;  /* timeout */
    }

    *out_sec = ntp_reply_sec;
    *out_usec = ntp_reply_usec;

    return 0;
}

/* ── Implement: ntp_sync ────────────────── */
int ntp_sync(void)
{
    kprintf("[ntp] ntp_sync: triggering NTP synchronization (stub)\n");
    return -EOPNOTSUPP;
}
/* ── Implement: ntp_update ────────────────── */
int ntp_update(uint64_t timestamp)
{
    if (timestamp == 0) {
        kprintf("[ntp] ntp_update: invalid timestamp 0\n");
        return -EINVAL;
    }
    kprintf("[ntp] ntp_update: timestamp=%llu (stub)\n",
            (unsigned long long)timestamp);
    return -EOPNOTSUPP;
}
/* ── Implement: ntp_set_server ────────────────── */
int ntp_set_server(const char *host)
{
    if (!host || !*host) {
        kprintf("[ntp] ntp_set_server: invalid hostname\n");
        return -EINVAL;
    }
    kprintf("[ntp] ntp_set_server: host=%s (stub)\n", host);
    return -EOPNOTSUPP;
}
