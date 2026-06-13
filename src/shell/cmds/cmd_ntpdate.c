/*
 * cmd_ntpdate.c — NTP client shell command
 *
 * Usage: ntpdate <server_ip_or_hostname>
 * Sets system time from NTP server.
 *
 * Uses net.h for network API and libc.h for syscalls.
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "net.h"
#include "libc.h"

/* NTP protocol constants (inline to avoid needing ntp.h in app code) */
#define NTP_PORT         123
#define NTP_VERSION      4
#define NTP_MODE_CLIENT  3
#define NTP_MODE_SERVER  4
#define NTP_UNIX_OFFSET  2208988800ULL

/* NTP packet header */
struct ntp_packet {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    int8_t   poll;
    int8_t   precision;
    int32_t  root_delay;
    int32_t  root_dispersion;
    uint32_t reference_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t rx_ts_sec;
    uint32_t rx_ts_frac;
    uint32_t tx_ts_sec;
    uint32_t tx_ts_frac;
} __attribute__((packed));

/* NTP reply state */
static volatile int ntp_reply_received = 0;
static volatile uint64_t ntp_reply_sec = 0;
static volatile uint32_t ntp_reply_usec = 0;

/* UDP handler for NTP replies */
static void handle_ntp_reply(uint32_t src_ip, uint16_t src_port,
                              const uint8_t *data, uint16_t len) {
    (void)src_ip;
    (void)src_port;

    if (len < sizeof(struct ntp_packet)) return;

    const struct ntp_packet *pkt = (const struct ntp_packet *)data;

    uint8_t mode = pkt->li_vn_mode & 0x07;
    uint8_t stratum = pkt->stratum;

    if (mode != NTP_MODE_SERVER) return;
    if (stratum == 0 || stratum > 15) return;

    uint32_t t3_sec = ntohl(pkt->tx_ts_sec);
    uint32_t t3_frac = ntohl(pkt->tx_ts_frac);

    /* Convert NTP timestamp (1900 epoch) to UNIX timestamp (1970 epoch) */
    ntp_reply_sec = (uint64_t)t3_sec - NTP_UNIX_OFFSET;
    ntp_reply_usec = (uint32_t)(((uint64_t)t3_frac * 1000000) >> 32);
    ntp_reply_received = 1;
}

/* SYS_ clock syscalls */
#define SYS_CLOCK_SETTIME 232

void cmd_ntpdate(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: ntpdate <server>\n");
        kprintf("  Query an NTP server and set system time\n");
        kprintf("  Example: ntpdate pool.ntp.org\n");
        return;
    }

    /* Skip leading whitespace */
    while (*args == ' ') args++;
    if (!*args) {
        kprintf("Usage: ntpdate <server>\n");
        return;
    }

    char host[256];
    int i = 0;
    while (*args && *args != ' ' && i < 255)
        host[i++] = *args++;
    host[i] = '\0';

    /* Resolve hostname to IP */
    uint32_t ip = net_dns_resolve(host);
    if (!ip) {
        /* Try parsing as dotted decimal */
        unsigned int a, b, c, d;
        if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            ip = (a << 24) | (b << 16) | (c << 8) | d;
        }
    }

    if (!ip) {
        kprintf("ntpdate: cannot resolve '%s'\n", host);
        return;
    }

    kprintf("ntpdate: querying %u.%u.%u.%u...\n",
            (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF, ip & 0xFF);

    /* Bind UDP handler */
    uint16_t sport = 0xCDEF;
    net_udp_bind(sport, handle_ntp_reply);

    /* Build NTP request */
    struct ntp_packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;
    pkt.poll = 4;

    /* Set transmit timestamp from approximate current time */
    uint64_t now_sec = libc_syscall(11, 0, 0, 0, 0, 0);  /* time(NULL) */
    pkt.orig_ts_sec = htonl((uint32_t)(now_sec + NTP_UNIX_OFFSET));
    pkt.tx_ts_sec = pkt.orig_ts_sec;

    /* Send query */
    net_udp_send(ip, sport, NTP_PORT, &pkt, sizeof(pkt));

    /* Wait for reply with timeout (~5 seconds polling) */
    int timeout = 500;  /* ~5 seconds at 100 Hz polling */
    while (!ntp_reply_received && timeout-- > 0) {
        libc_syscall(35, 10, 0, 0, 0, 0);  /* usleep(10000) approx */
    }

    net_udp_bind(sport, NULL);

    if (!ntp_reply_received) {
        kprintf("ntpdate: no reply from %s (timeout)\n", host);
        return;
    }

    /* Set system time via clock_settime syscall */
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } ts;
    ts.tv_sec = ntp_reply_sec;
    ts.tv_nsec = (uint64_t)ntp_reply_usec * 1000;

    int rc = (int)libc_syscall(SYS_CLOCK_SETTIME, 0,
                                (uint64_t)(uintptr_t)&ts, 0, 0, 0);
    if (rc < 0) {
        kprintf("ntpdate: failed to set time (error %d)\n", rc);
        return;
    }

    kprintf("ntpdate: time set to %llu.%06u UTC\n",
            (unsigned long long)ntp_reply_sec, ntp_reply_usec);
}

void ntpdate_init(void) {
    kprintf("[OK] cmd_ntpdate: NTP client ready\n");
}
