#ifndef NTP_H
#define NTP_H

#include "types.h"

/* NTP protocol constants */
#define NTP_PORT         123
#define NTP_VERSION      4
#define NTP_MODE_CLIENT  3
#define NTP_MODE_SERVER  4

/* NTP timestamp (seconds + fraction, 64-bit fixed point) */
struct ntp_timestamp {
    uint32_t seconds;     /* seconds since 1900-01-01 */
    uint32_t fraction;    /* fractional seconds (2^-32 units) */
} __attribute__((packed));

/* NTP packet header (first 48 bytes of an NTP packet) */
struct ntp_packet {
    uint8_t  li_vn_mode;       /* Leap Indicator (2), Version (3), Mode (3) */
    uint8_t  stratum;          /* Stratum level */
    int8_t   poll;             /* Poll interval (log2 seconds) */
    int8_t   precision;        /* Precision (log2 seconds) */
    int32_t  root_delay;       /* Root delay (fixed point, NTP short format) */
    int32_t  root_dispersion;  /* Root dispersion (fixed point, NTP short format) */
    uint32_t reference_id;     /* Reference ID */
    struct ntp_timestamp ref_ts;       /* Reference timestamp */
    struct ntp_timestamp orig_ts;      /* Origin timestamp (t1) */
    struct ntp_timestamp rx_ts;        /* Receive timestamp (t2) */
    struct ntp_timestamp tx_ts;        /* Transmit timestamp (t3) */
} __attribute__((packed));

/* Convert NTP timestamp (seconds since 1900) to UNIX (seconds since 1970) */
#define NTP_UNIX_OFFSET 2208988800ULL  /* seconds between 1900-01-01 and 1970-01-01 */

static inline uint64_t ntp_to_unix_sec(uint32_t ntp_sec) {
    return (uint64_t)ntp_sec - NTP_UNIX_OFFSET;
}

static inline uint32_t unix_to_ntp_sec(uint64_t unix_sec) {
    return (uint32_t)(unix_sec + NTP_UNIX_OFFSET);
}

/**
 * ntp_request - Send NTP query and parse response
 * @server_ip:  IP address of NTP server (host byte order)
 * @out_sec:    On success, set to UNIX timestamp (seconds)
 * @out_usec:   On success, set to microsecond fraction
 *
 * Returns 0 on success, -1 on timeout/error.
 */
int ntp_request(uint32_t server_ip, uint64_t *out_sec, uint32_t *out_usec);

#endif /* NTP_H */
