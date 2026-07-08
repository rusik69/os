/* net_tcp.c — TCP connection management */
#define KERNEL_INTERNAL

#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "scheduler.h"
#include "sha256.h"
#include "syscall.h"   /* for prng_rand64 */
#include "tcp_bbr.h"   /* BBR congestion control (Item 157) */
#include "spinlock.h"  /* spinlock_t, SPINLOCK_INIT */
#include "socket.h"    /* struct sockaddr_in */
#include "mptcp.h"     /* MPTCP options, MP_CAPABLE handling */

/* TCP connection table lock — protects tcp_conns[] */
static spinlock_t tcp_lock = SPINLOCK_INIT;

/* TCP connection table entry timeout for 2*MSL (60 seconds at ~100 Hz) */
#define TCP_TIME_WAIT_MSL_TICKS 6000

/* ── TCP Fast Open (TFO) — RFC 7413 (Item 159) ──────────────────────── */
#define TFO_COOKIE_LEN     8   /* TFO cookie is 8 bytes */
#define TFO_CACHE_SIZE     16  /* max cached destinations */
#define TCP_RXBUF_SIZE     4096 /* receive buffer size */

/* TFO cookie cache entry */
struct tfo_cookie_entry {
    uint32_t ip;                            /* destination IP */
    uint8_t  cookie[TFO_COOKIE_LEN];        /* cached cookie */
    uint64_t last_used;                     /* for LRU eviction */
};

/* TFO cookie cache */
static struct tfo_cookie_entry tfo_cache[TFO_CACHE_SIZE];
static int tfo_cache_count = 0;

/* TFO secret key — generated once at boot via prng_rand64() */
static uint64_t tfo_secret[2];

/* ── Generate a TFO cookie for a client-server pair ───────────────
 * Cookie = first 8 bytes of SHA256(client_ip | server_ip | port | secret)
 * This matches the standard Linux-style cookie generation approach.
 * The 16-byte secret is generated at boot, preventing forgery. */
static void tfo_generate_cookie(uint32_t client_ip, uint32_t server_ip,
                                 uint16_t server_port, uint8_t cookie[8])
{
    struct sha256_ctx ctx;
    uint8_t digest[32];
    uint32_t ncip = htonl(client_ip);
    uint32_t nsip = htonl(server_ip);
    uint16_t nsp  = htons(server_port);

    sha256_init(&ctx);
    sha256_update(&ctx, &ncip, 4);
    sha256_update(&ctx, &nsip, 4);
    sha256_update(&ctx, &nsp, 2);
    sha256_update(&ctx, tfo_secret, sizeof(tfo_secret));
    sha256_final(digest, &ctx);

    memcpy(cookie, digest, 8);
}

/* Validate a received TFO cookie matches what we would generate */
static int tfo_validate_cookie(uint32_t client_ip, uint32_t server_ip,
                                uint16_t server_port, const uint8_t cookie[8])
{
    uint8_t expected[8];
    tfo_generate_cookie(client_ip, server_ip, server_port, expected);
    return (memcmp(expected, cookie, 8) == 0);
}

/* ── Look up a cached TFO cookie for the given destination IP.
 * Returns 1 and copies cookie if found, 0 on miss. */
static int tfo_cache_lookup(uint32_t ip, uint8_t cookie[8])
{
    for (int i = 0; i < tfo_cache_count; i++) {
        if (tfo_cache[i].ip == ip) {
            memcpy(cookie, tfo_cache[i].cookie, 8);
            tfo_cache[i].last_used = timer_get_ticks();
            return 1;
        }
    }
    return 0;
}

/* Store (or update) a TFO cookie for the given destination IP.
 * Uses LRU eviction if the cache is full. */
static void tfo_cache_store(uint32_t ip, const uint8_t cookie[8])
{
    /* Update if already in cache */
    for (int i = 0; i < tfo_cache_count; i++) {
        if (tfo_cache[i].ip == ip) {
            memcpy(tfo_cache[i].cookie, cookie, 8);
            tfo_cache[i].last_used = timer_get_ticks();
            return;
        }
    }

    /* Add new entry if space remains */
    if (tfo_cache_count < TFO_CACHE_SIZE) {
        tfo_cache[tfo_cache_count].ip = ip;
        memcpy(tfo_cache[tfo_cache_count].cookie, cookie, 8);
        tfo_cache[tfo_cache_count].last_used = timer_get_ticks();
        tfo_cache_count++;
        return;
    }

    /* Evict the LRU entry */
    int lru_idx = 0;
    uint64_t oldest = tfo_cache[0].last_used;
    for (int i = 1; i < TFO_CACHE_SIZE; i++) {
        if (tfo_cache[i].last_used < oldest) {
            oldest = tfo_cache[i].last_used;
            lru_idx = i;
        }
    }
    tfo_cache[lru_idx].ip = ip;
    memcpy(tfo_cache[lru_idx].cookie, cookie, 8);
    tfo_cache[lru_idx].last_used = timer_get_ticks();
}

/* Initialize TFO — generate the secret key (called at boot) */
void tcp_tfo_init(void)
{
    tfo_secret[0] = prng_rand64();
    tfo_secret[1] = prng_rand64();
    kprintf("[TFO] TCP Fast Open initialized\n");
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  Fixed-point integer cube root (Newton-Raphson, rounds toward zero)
 *  Returns floor(cbrt(a)) for a > 0.  Uses at most 10 iterations.
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

/* ── Forward declarations for Nagle/Delayed ACK helpers ────────── */
static void tcp_flush_delayed_ack(struct tcp_conn *c);
static void tcp_flush_nagle(struct tcp_conn *c);

/* ── PRR (Proportional Rate Reduction, RFC 6937) — Item 158 ───────
 * During fast recovery, compute the number of bytes we are allowed
 * to send in response to a newly-delivered segment.
 *
 * PRR maintains two counters during recovery:
 *   prr_delivered — total bytes newly ACKed since recovery started
 *   prr_out       — total bytes sent since recovery started
 *
 * On each ACK that delivers new data during recovery:
 *   1. Update prr_delivered by the amount of newly delivered data.
 *   2. Compute the allowance:
 *        limit = max(0, DeliveredData - (prr_delivered - prr_out)) + MSS
 *      This ensures that the amount of data sent is proportional to
 *      the amount delivered, keeping the window at the target (ssthresh).
 *   3. We may send up to 'limit' bytes.
 *
 * Returns the number of bytes we may send (0 if none). */
static uint32_t prr_send_allowed(struct tcp_conn *c, uint32_t delivered_data)
{
    if (!c->in_recovery)
        return 0;

    c->prr_delivered += delivered_data;

    /* PRR basic formula (RFC 6937 §3.1):
     *   limit = max(0, DeliveredData - (prr_delivered - prr_out)) * MSS
     * In our implementation:
     *   limit = max(0, (int)delivered_data - ((int)c->prr_delivered - (int)c->prr_out))
     *           + MSS (to allow 1 segment per ACK minimum) */
    int32_t delta = (int32_t)delivered_data
                  - ((int32_t)c->prr_delivered - (int32_t)c->prr_out);
    if (delta < 0)
        delta = 0;

    /* Add 1 MSS so we can send at least one segment per ACK
     * (this implements "bound with one segment" from RFC 6937 §3.2) */
    uint32_t limit = (uint32_t)delta + 1400;

    /* Clamp to what the congestion window allows:
     * flightsize = approximate bytes in flight (tx_unacked_len)
     * We can send at most cwnd * 1460 - flightsize */
    uint32_t flightsize = c->tx_unacked_len;
    uint32_t cwnd_bytes = c->cwnd * 1460;
    if (limit + flightsize > cwnd_bytes) {
        if (cwnd_bytes > flightsize)
            limit = cwnd_bytes - flightsize;
        else
            limit = 0;
    }

    return limit;
}

/* ── SYN cookies (RFC 4987) ────────────────────────────────────────
 *
 * When the TCP connection table is full (SYN flood), we avoid
 * allocating a connection for every SYN.  Instead we encode a
 * cryptographic cookie in the SYN-ACK's initial sequence number
 * and only create a full connection when the client returns the
 * ACK that completes the three-way handshake.
 *
 * Cookie format (32-bit sequence number):
 *   bits 0-5:   MSS index (0..63, indexed into a small table)
 *   bits 6-31:  SHA-256-based hash over (saddr, sport, daddr, dport, secret)
 *
 * The hash uses a secret key that allows the receiver to validate the
 * cookie when the ACK arrives — no per-connection state is needed
 * until the handshake completes.
 */

/* A small table of common MSS values for encoding in the cookie.
 * Index 0 is a reasonable default (536 = IPv4 minimum). */
#define SYN_COOKIE_MSS_TABLE_SIZE 8
static const uint16_t syn_cookie_mss_table[SYN_COOKIE_MSS_TABLE_SIZE] = {
    536,   /* IPv4 minimum reassembly buffer */
    1460,  /* typical Ethernet + no options */
    1440,  /* Ethernet + timestamp */
    1400,  /* conservative */
    1300,  /* PPPoE / VPN */
    1200,  /* tunnel / low MTU */
    1024,  /* safe fallback */
    896    /* dialup / low-end */
};

/* 16-byte secret key for SYN cookie computation.
 * Initialised at boot from the PRNG and periodically refreshed. */
static uint8_t syn_cookie_secret[16];
static int syn_cookie_seeded = 0;

/* Seed the SYN cookie secret from the kernel PRNG */
static void syn_cookie_seed(void) {
    if (syn_cookie_seeded) return;
    for (int i = 0; i < 16; i++)
        syn_cookie_secret[i] = (uint8_t)(prng_rand64() & 0xFF);
    syn_cookie_seeded = 1;
}

/* Compute a SYN cookie for the given 4-tuple + MSS.
 * Returns a 32-bit value to use as the SYN-ACK initial sequence number. */
static uint32_t compute_syn_cookie(uint32_t saddr, uint16_t sport,
                                   uint32_t daddr, uint16_t dport,
                                   uint16_t mss)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    struct sha256_ctx ctx;

    /* Seed the secret on first use */
    syn_cookie_seed();

    /* Build the hash input: 4-tuple + secret */
    sha256_init(&ctx);
    sha256_update(&ctx, &saddr, sizeof(saddr));
    sha256_update(&ctx, &sport, sizeof(sport));
    sha256_update(&ctx, &daddr, sizeof(daddr));
    sha256_update(&ctx, &dport, sizeof(dport));
    sha256_update(&ctx, syn_cookie_secret, sizeof(syn_cookie_secret));
    sha256_final(digest, &ctx);

    /* Encode MSS index in lower 6 bits */
    uint32_t mss_index = 0;
    for (uint32_t i = 0; i < SYN_COOKIE_MSS_TABLE_SIZE; i++) {
        if (syn_cookie_mss_table[i] == mss) {
            mss_index = i;
            break;
        } else if (i > 0 && syn_cookie_mss_table[i] > mss) {
            /* Pick the next higher value (conservative) */
            mss_index = i;
            break;
        }
    }
    if (mss_index >= SYN_COOKIE_MSS_TABLE_SIZE)
        mss_index = SYN_COOKIE_MSS_TABLE_SIZE - 1;

    /* Combine hash bits (26 bits from first 4 bytes) with MSS index (6 bits) */
    uint32_t hash_part = (uint32_t)digest[0]
                       | ((uint32_t)digest[1] << 8)
                       | ((uint32_t)digest[2] << 16)
                       | ((uint32_t)(digest[3] & 0x03) << 24);
    return (hash_part & ~0x3F) | mss_index;
}

/* Validate a SYN cookie and extract the encoded MSS value.
 * Returns the MSS on success, or 0 if the cookie is invalid. */
static uint16_t check_syn_cookie(uint32_t cookie, uint32_t saddr,
                                  uint16_t sport, uint32_t daddr,
                                  uint16_t dport)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    struct sha256_ctx ctx;

    if (!syn_cookie_seeded) return 0;

    /* Recompute the hash */
    sha256_init(&ctx);
    sha256_update(&ctx, &saddr, sizeof(saddr));
    sha256_update(&ctx, &sport, sizeof(sport));
    sha256_update(&ctx, &daddr, sizeof(daddr));
    sha256_update(&ctx, &dport, sizeof(dport));
    sha256_update(&ctx, syn_cookie_secret, sizeof(syn_cookie_secret));
    sha256_final(digest, &ctx);

    /* Extract the expected hash part */
    uint32_t expected_hash = (uint32_t)digest[0]
                           | ((uint32_t)digest[1] << 8)
                           | ((uint32_t)digest[2] << 16)
                           | ((uint32_t)(digest[3] & 0x03) << 24);

    /* Bits 6-31 must match */
    if ((cookie & ~0x3F) != (expected_hash & ~0x3F))
        return 0;

    /* Decode MSS index from lower 6 bits */
    uint32_t mss_index = cookie & 0x3F;
    if (mss_index >= SYN_COOKIE_MSS_TABLE_SIZE)
        return 0;

    return syn_cookie_mss_table[mss_index];
}

uint16_t net_transport_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                                 const void *data, uint16_t data_len) {
    struct tcp_pseudo pseudo;
    pseudo.src_ip = htonl(src_ip);
    pseudo.dst_ip = htonl(dst_ip);
    pseudo.zero = 0;
    pseudo.protocol = protocol;
    pseudo.tcp_len = htons(data_len);

    uint32_t sum = 0;
    const uint8_t *pb = (const uint8_t *)&pseudo;
    for (int i = 0; i < (int)sizeof(pseudo); i += 2) {
        uint16_t w; __builtin_memcpy(&w, pb + i, 2); sum += w;
    }
    pb = (const uint8_t *)data;
    int len = data_len;
    while (len > 1) { uint16_t w; __builtin_memcpy(&w, pb, 2); sum += w; pb += 2; len -= 2; }
    if (len == 1) sum += *pb;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~(uint16_t)sum;
}

void send_tcp(struct tcp_conn *conn, uint8_t flags, const void *data, uint16_t data_len) {
    uint8_t buf[1500];
    struct tcp_header *tcp = (struct tcp_header *)buf;
    memset(tcp, 0, sizeof(*tcp));
    tcp->src_port = htons(conn->local_port);
    tcp->dst_port = htons(conn->remote_port);
    tcp->seq_num = htonl(conn->our_seq);
    tcp->ack_num = htonl(conn->their_seq);

    /*
     * Delayed ACK piggyback (RFC 1122 §4.2.3.2):
     * If we have a pending delayed ACK and this segment carries data,
     * piggyback the ACK flag instead of sending a separate pure ACK.
     */
    if (conn->delayed_ack_pending && (data_len > 0 || (flags & (TCP_SYN | TCP_FIN | TCP_RST)))) {
        flags |= TCP_ACK;
        tcp->ack_num = htonl(conn->their_seq);
        conn->delayed_ack_pending = 0;
    }

    uint16_t opt_len = 0;
    uint8_t *opts = buf + sizeof(struct tcp_header);

    /* ── TCP options on SYN ──────────────────────────────────────
     * SACK-permitted for selective ACK (RFC 2018).
     * TFO cookie option (RFC 7413): server emits on SYN-ACK;
     * client emits on SYN when it has a cached cookie. */
    if (flags & TCP_SYN) {
        /* SACK-permitted (kind 4, length 2) */
        opts[opt_len++] = 4;
        opts[opt_len++] = 2;

        /* TCP Fast Open cookie option (kind 34) */
        if (flags & TCP_ACK) {
            /* Server-side SYN-ACK: always generate and attach a cookie
             * so the client can use TFO on its next connection. */
            uint8_t tfo_cookie[TFO_COOKIE_LEN];
            tfo_generate_cookie(conn->remote_ip, net_our_ip,
                                conn->local_port, tfo_cookie);
            opts[opt_len++] = 34;      /* TFO option kind */
            opts[opt_len++] = 10;      /* len: kind(1) + len(1) + cookie(8) */
            memcpy(opts + opt_len, tfo_cookie, 8);
            opt_len += 8;
        } else if (conn->tfo_cookie_present) {
            /* Client-side SYN: attach cached cookie so server can
             * accept data immediately without the full handshake. */
            opts[opt_len++] = 34;      /* TFO option kind */
            opts[opt_len++] = 10;      /* length */
            memcpy(opts + opt_len, conn->tfo_cookie, 8);
            opt_len += 8;
        }

        /* ── MPTCP MP_CAPABLE option ──────────────────────────────
         * If this connection is MPTCP-associated, include the
         * MP_CAPABLE option with our 64-bit key on both SYN (client)
         * and SYN-ACK (server).  The option kind is 30 (TCPOPT_MPTCP),
         * subtype 0 (MPTCP_CAPABLE). */
        if (conn->mptcp_token != 0) {
            uint16_t mptcp_opt_len = MPTCP_CAPABLE_SYN_LEN;
            int ret = mptcp_build_capable_syn(opts + opt_len,
                                               &mptcp_opt_len,
                                               conn->mptcp_snd_key);
            if (ret == 0) {
                opt_len += mptcp_opt_len;
            }
        }
    }

    uint16_t hdr_len = sizeof(struct tcp_header) + opt_len;
    tcp->data_off = (uint8_t)((hdr_len / 4) << 4);
    tcp->flags = flags;
    tcp->window = htons(8192);
    if (data && data_len > 0)
        memcpy(buf + hdr_len, data, data_len);
    tcp->checksum = 0;
    tcp->checksum = net_transport_checksum(net_our_ip, conn->remote_ip, IP_PROTO_TCP,
                                           buf, hdr_len + data_len);

    send_ip(conn->remote_ip, IP_PROTO_TCP, buf, hdr_len + data_len);
}

static struct tcp_listener *find_listener(uint16_t port) {
    for (int i = 0; i < net_num_listeners; i++)
        if (net_listeners[i].port == port) return &net_listeners[i];
    return NULL;
}

static int find_conn(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].state != TCP_CLOSED &&
            tcp_conns[i].state != TCP_TIME_WAIT && /* skip TIME_WAIT for reuse */
            tcp_conns[i].remote_ip == remote_ip &&
            tcp_conns[i].remote_port == remote_port &&
            tcp_conns[i].local_port == local_port)
            return i;
    }
    return -1;
}

static int alloc_conn(void) {
    for (int i = 0; i < MAX_TCP_CONNS; i++)
        if (tcp_conns[i].state == TCP_CLOSED) return i;
    return -1;
}

void handle_tcp(struct ip_header *ip_hdr, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct tcp_header)) return;
    struct tcp_header *tcp = (struct tcp_header *)payload;

    /* Verify TCP checksum */
    uint32_t csum_src = ntohl(ip_hdr->src_ip);
    uint32_t csum_dst = ntohl(ip_hdr->dst_ip);
    uint16_t saved_csum = tcp->checksum;
    if (saved_csum == 0) return; /* RFC 793: TCP requires a valid checksum; 0 is never valid for TCP */
    tcp->checksum = 0;
    if (net_transport_checksum(csum_src, csum_dst, IP_PROTO_TCP, payload, len) != saved_csum)
        return;

    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t remote_ip = ntohl(ip_hdr->src_ip);
    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = tcp->flags;
    uint16_t hdr_len = (tcp->data_off >> 4) * 4;
    if (hdr_len < sizeof(struct tcp_header) || hdr_len > len) return;
    uint16_t data_len = len - hdr_len;
    const uint8_t *data = payload + hdr_len;

    int conn_id = find_conn(remote_ip, src_port, dst_port);

    /* Parse TCP options — extract SACK blocks if present (only for established conns) */
    if (conn_id >= 0) {
        int opt_offset = sizeof(struct tcp_header);
        while (opt_offset + 1 < (int)hdr_len) {
            uint8_t kind = payload[opt_offset];
            if (kind == 0) break; /* End of options */
            if (kind == 1) { opt_offset++; continue; } /* NOP */
            if (opt_offset + 1 >= (int)hdr_len) break;
            uint8_t olen = payload[opt_offset + 1];
            if (olen < 2 || opt_offset + olen > (int)hdr_len) break;
            if (kind == 5) { /* SACK option */
                int num_blocks = (olen - 2) / 8;
                if (num_blocks > TCP_MAX_SACK_BLOCKS) num_blocks = TCP_MAX_SACK_BLOCKS;
                struct tcp_conn *sc = &tcp_conns[conn_id];
                memset(sc->sack_blocks, 0, sizeof(sc->sack_blocks));
                for (int sb = 0; sb < num_blocks; sb++) {
                    int off = opt_offset + 2 + sb * 8;
                    if (off + 8 <= (int)hdr_len) {
                        sc->sack_blocks[sb].left = ntohl(*(uint32_t*)(payload + off));
                        sc->sack_blocks[sb].right = ntohl(*(uint32_t*)(payload + off + 4));
                    }
                }
                sc->sack_pending = 1;
                /* ── NewReno SACK scoreboard update (RFC 6675) ──────────
                 * If this connection uses NewReno congestion control,
                 * update the SACK scoreboard with the newly parsed blocks.
                 * The scoreboard tracks high_sacked, pipe, and scoring
                 * state for enhanced loss detection during recovery. */
                if (sc->cc_algo == 3) {
                    newreno_sack_scoreboard_update(&sc->newreno,
                                                   sc->sack_blocks,
                                                   num_blocks,
                                                   sc->last_ack,
                                                   sc->tx_unacked_len,
                                                   sc->tx_unacked_seq);
                }
                /* ── RACK: update fwd_mark from highest SACK right edge ──
                 * SACK informs us that the receiver has data beyond the
                 * cumulative ACK.  Track the highest SACK-reported delivery
                 * to improve loss detection accuracy. */
                uint32_t max_sack = 0;
                for (int sb = 0; sb < num_blocks; sb++) {
                    int off = opt_offset + 2 + sb * 8;
                    if (off + 8 <= (int)hdr_len) {
                        uint32_t right = ntohl(*(uint32_t*)(payload + off + 4));
                        if ((int32_t)(right - max_sack) > 0)
                            max_sack = right;
                    }
                }
                if (max_sack > 0 && (int32_t)(max_sack - sc->rack_fwd_mark) > 0) {
                    sc->rack_fwd_mark = max_sack;
                    sc->rack_fwd_tick = timer_get_ticks();
                }
            } else if (kind == 19) { /* TCP MD5 Signature option */
                struct tcp_conn *sc = &tcp_conns[conn_id];
                sc->md5_enabled = 1;
                /* Option 19 format: kind(1) + len(1) + digest(16) */
                if (olen >= 18 && opt_offset + 2 + 16 <= (int)hdr_len) {
                    __builtin_memcpy(sc->md5_digest, payload + opt_offset + 2, 16);
                }
            } else if (kind == 34) { /* TCP Fast Open (TFO) Cookie option */
                struct tcp_conn *sc = &tcp_conns[conn_id];
                sc->tfo_cookie_present = 1;
                /* Option 34 format: kind(1) + len(1) + cookie(0-8 bytes) */
                int cookie_len = olen - 2;
                if (cookie_len > 8) cookie_len = 8;
                if (cookie_len > 0 && opt_offset + 2 + cookie_len <= (int)hdr_len) {
                    __builtin_memcpy(sc->tfo_cookie, payload + opt_offset + 2, cookie_len);
                }
            } else if (kind == 30) { /* MPTCP option (kind 30, TCPOPT_MPTCP) */
                /* Determine MPTCP subtype and dispatch */
                if (olen >= MPTCP_FASTCLOSE_LEN &&
                    (payload[opt_offset + 2] >> 4) == MPTCP_FASTCLOSE) {
                    /* Handle MP_FASTCLOSE — immediately close all subflows */
                    mptcp_handle_fastclose(conn_id, payload + opt_offset, olen);
                } else {
                    /* Handle MP_CAPABLE option for MPTCP handshake */
                    mptcp_handle_capable(conn_id, payload + opt_offset, olen);
                }
            }
            opt_offset += olen;
        }
    }

    if (conn_id < 0 && (flags & TCP_SYN)) {
        struct tcp_listener *l = find_listener(dst_port);
        if (!l) {
            struct tcp_conn tmp = {0};
            tmp.remote_ip = remote_ip;
            tmp.remote_port = src_port;
            tmp.local_port = dst_port;
            tmp.our_seq = ack;
            tmp.their_seq = seq + 1;
            send_tcp(&tmp, TCP_RST | TCP_ACK, NULL, 0);
            return;
        }

        /* Parse TCP options from the SYN — extract MSS and TFO cookie */
        uint16_t client_mss = 1460; /* default MSS for Ethernet */
        int syn_has_tfo = 0;
        uint8_t syn_tfo_cookie[8];
        int syn_has_mptcp = 0;
        uint8_t syn_mptcp_key[8];
        {
            int opt_off = sizeof(struct tcp_header);
            while (opt_off + 1 < (int)hdr_len) {
                uint8_t kind = payload[opt_off];
                if (kind == 0) break; /* End of options */
                if (kind == 1) { opt_off++; continue; } /* NOP */
                if (opt_off + 1 >= (int)hdr_len) break;
                uint8_t olen = payload[opt_off + 1];
                if (olen < 2 || opt_off + olen > (int)hdr_len) break;
                if (kind == 2 && olen == 4) {
                    /* MSS option: 2 bytes of MSS value */
                    client_mss = (uint16_t)payload[opt_off + 2] << 8
                               | (uint16_t)payload[opt_off + 3];
                } else if (kind == 34) {
                    /* TCP Fast Open (TFO) Cookie option (kind 34) */
                    int cookie_len = olen - 2;
                    if (cookie_len > 8) cookie_len = 8;
                    if (cookie_len > 0 && opt_off + 2 + cookie_len <= (int)hdr_len) {
                        syn_has_tfo = 1;
                        memset(syn_tfo_cookie, 0, 8);
                        memcpy(syn_tfo_cookie, payload + opt_off + 2, cookie_len);
                    }
                } else if (kind == 30) {
                    /* MPTCP option (kind 30, TCPOPT_MPTCP) — MP_CAPABLE on SYN */
                    if (olen >= MPTCP_CAPABLE_SYN_LEN &&
                        (payload[opt_off + 2] >> 4) == MPTCP_CAPABLE) {
                        syn_has_mptcp = 1;
                        memset(syn_mptcp_key, 0, 8);
                        memcpy(syn_mptcp_key, payload + opt_off + 4, 8);
                    }
                }
                opt_off += olen;
            }
        }

        conn_id = alloc_conn();
        if (conn_id < 0) {
            /* ── Connection table full → use SYN cookie (RFC 4987) ── */
            uint32_t cookie = compute_syn_cookie(remote_ip, src_port,
                                                  ip_hdr->dst_ip, dst_port,
                                                  client_mss);
            /* Send SYN-ACK with the cookie as our initial sequence number.
             * We build the reply manually since send_tcp expects a full
             * tcp_conn struct. */
            struct tcp_conn tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.remote_ip = remote_ip;
            tmp.remote_port = src_port;
            tmp.local_port = dst_port;
            tmp.our_seq = cookie;
            tmp.their_seq = seq + 1;
            send_tcp(&tmp, TCP_SYN | TCP_ACK, NULL, 0);
            return;
        }

        struct tcp_conn *c = &tcp_conns[conn_id];
        c->state = TCP_SYN_RECEIVED;
        c->remote_ip = remote_ip;
        c->remote_port = src_port;
        c->local_port = dst_port;
        c->our_seq = 1000 + net_ip_id_counter * 1000;
        c->their_seq = seq + 1;
        c->their_window = ntohs(tcp->window);
        c->rxlen = 0;    /* reset stale state from previous use */
        c->rx_fin = 0;
        c->cwnd = 1;
        c->ssthresh = 65535;
        c->dupack_count = 0;
        c->srtt = 0;
        c->rttvar = 0;
        c->tx_unacked_len  = 0;
        c->tx_unacked_seq  = 0;
        c->last_send_tick  = 0;
        c->retrans_count   = 0;
        c->rto             = 30;   /* 3000ms initial RTO (100Hz) */
        c->tcp_nodelay     = 0;
        c->tcp_cork        = 0;
        c->keepalive       = 0;
        c->keepalive_interval = 500;
        c->keepalive_probes = 0;
        c->keepalive_probes_max = 3;
        c->last_activity_tick = 0;
        c->md5_enabled = 0;
        c->tfo_cookie_present = 0;
        memset(c->md5_digest, 0, sizeof(c->md5_digest));
        memset(c->tfo_cookie, 0, sizeof(c->tfo_cookie));
        memset(c->sack_blocks, 0, sizeof(c->sack_blocks));
        c->sack_pending = 0;
        c->delayed_ack_pending = 0;
        c->delayed_ack_tick = 0;
        c->nagle_buf_len = 0;
        c->time_wait_deadline = 0;
        /* Congestion control initialization */
        cubic_init(&c->cubic);
        newreno_init(&c->newreno);
        c->cc_algo = 0;  /* 0 = CUBIC (default), 1 = BBR, 2 = BBRv3, 3 = NewReno */
        /* RACK (Recent ACKnowledgment) loss detection initialization */
        c->rack_fwd_mark    = 0;
        c->rack_fwd_tick    = 0;
        c->rack_reo_wnd     = 1;   /* default: 1 tick reordering window */
        c->rack_min_rtt     = 0;
        /* Nagle / Delayed ACK initialization */
        c->delayed_ack_pending = 0;
        c->nagle_buf_len = 0;

        /* MPTCP initialization */
        c->mptcp_token = 0;
        c->mptcp_rcv_key_valid = 0;
        memset(c->mptcp_snd_key, 0, 8);
        memset(c->mptcp_rcv_key, 0, 8);

        /* ── MPTCP MP_CAPABLE on SYN ─────────────────────────────────
         * If the client included MP_CAPABLE in the SYN, we are dealing
         * with an MPTCP-capable connection.  Store the peer's key and
         * create an MPTCP connection so the SYN-ACK includes our key. */
        if (syn_has_mptcp) {
            memcpy(c->mptcp_rcv_key, syn_mptcp_key, 8);
            c->mptcp_rcv_key_valid = 1;
            int mptcp_token = mptcp_create();
            if (mptcp_token >= 0) {
                int ret = mptcp_associate(conn_id, (uint32_t)mptcp_token);
                if (ret == 0) {
                    kprintf("[TCP] MP_CAPABLE SYN on conn %d, MPTCP token=%d\n",
                            conn_id, mptcp_token);
                }
            }
        }

        /* ── TCP Fast Open: validate cookie and process data-in-SYN ────
         * If the client provided a valid TFO cookie AND payload data,
         * we can process the data immediately and transition to ESTABLISHED
         * without waiting for the ACK that completes the three-way handshake.
         * This saves one RTT on repeat connections (RFC 7413). */
        if (syn_has_tfo && data_len > 0 &&
            tfo_validate_cookie(remote_ip, ip_hdr->dst_ip,
                                dst_port, syn_tfo_cookie)) {
            /* Valid TFO cookie — accept the data in the SYN */
            /* Validate that data fits within our advertised window (RFC 7413 §4.2) */
            uint32_t their_win = c->their_window ? c->their_window : 65535;
            if (data_len > their_win) {
                kprintf("[TCP] TFO data exceeds window: data_len=%u, window=%u\n",
                        data_len, their_win);
                send_tcp(c, TCP_SYN | TCP_ACK, NULL, 0);
                c->our_seq++;
                return;
            }
            uint16_t copy_len = data_len;
            if (copy_len > TCP_RXBUF_SIZE)
                copy_len = TCP_RXBUF_SIZE;

            memcpy(c->rxbuf, data, copy_len);
            c->rxlen = copy_len;
            c->their_seq = seq + 1 + copy_len; /* account for data in SYN */
            c->state = TCP_ESTABLISHED;

            kprintf("[TFO] Valid cookie from %d.%d.%d.%d:%d, "
                    "accepted %u bytes in SYN\n",
                    (remote_ip >> 24) & 0xFF, (remote_ip >> 16) & 0xFF,
                    (remote_ip >> 8) & 0xFF, remote_ip & 0xFF,
                    src_port, copy_len);

            /* Notify the listener about the new connection */
            send_tcp(c, TCP_SYN | TCP_ACK, NULL, 0);
            c->our_seq++;

            if (l && l->on_connect) {
                l->on_connect(conn_id);
            } else if (l && l->accept_count < ACCEPT_QUEUE_SIZE) {
                l->accept_queue[l->accept_tail] = conn_id;
                l->accept_tail = (l->accept_tail + 1) % ACCEPT_QUEUE_SIZE;
                l->accept_count++;
            }
            return;
        }

        send_tcp(c, TCP_SYN | TCP_ACK, NULL, 0);
        c->our_seq++;
        return;
    }

    if (conn_id < 0) {
        /* ── No matching connection — check for SYN cookie ACK ──
         * If this is a pure ACK (completing a three-way handshake
         * initiated via SYN cookies), validate the cookie and create
         * a full connection on the fly. */
        if ((flags & TCP_ACK) && !(flags & TCP_SYN) && data_len == 0) {
            uint16_t decoded_mss = check_syn_cookie(ack - 1, remote_ip,
                                                     src_port, ip_hdr->dst_ip,
                                                     dst_port);
            if (decoded_mss > 0) {
                /* Valid SYN cookie — allocate a connection */
                conn_id = alloc_conn();
                if (conn_id >= 0) {
                    struct tcp_conn *c = &tcp_conns[conn_id];
                    memset(c, 0, sizeof(*c));
                    c->state = TCP_ESTABLISHED;
                    c->remote_ip = remote_ip;
                    c->remote_port = src_port;
                    c->local_port = dst_port;
                    c->our_seq = ack;           /* matches what we sent */
                    c->their_seq = seq + 1;      /* next expected from peer */
                    c->their_window = ntohs(tcp->window);
                    c->cwnd = 1;
                    c->ssthresh = 65535;
                    c->rto = 30;
                    /* RACK (Recent ACKnowledgment) loss detection init */
                    c->rack_fwd_mark    = 0;
                    c->rack_fwd_tick    = 0;
                    c->rack_reo_wnd     = 1;
                    c->rack_min_rtt     = 0;
                    memset(c->sack_blocks, 0, sizeof(c->sack_blocks));
                    c->sack_pending = 0;

                    struct tcp_listener *l = find_listener(dst_port);
                    if (l) {
                        if (l->on_connect) {
                            l->on_connect(conn_id);
                        } else if (l->accept_count < ACCEPT_QUEUE_SIZE) {
                            l->accept_queue[l->accept_tail] = conn_id;
                            l->accept_tail = (l->accept_tail + 1) % ACCEPT_QUEUE_SIZE;
                            l->accept_count++;
                        } else {
                            /* Accept queue full — reject */
                            send_tcp(c, TCP_RST, NULL, 0);
                            c->state = TCP_CLOSED;
                        }
                    }
                    return;
                }
            }
        }
        return;
    }

    struct tcp_conn *c = &tcp_conns[conn_id];
    struct tcp_listener *l = find_listener(c->local_port);

    if (c->state == TCP_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            c->their_seq = seq + 1;
            c->our_seq = ack;
            c->state = TCP_ESTABLISHED;
            /* Cache TFO cookie received in SYN-ACK for future TFO connections */
            if (c->tfo_cookie_present) {
                tfo_cache_store(c->remote_ip, c->tfo_cookie);
            }
            send_tcp(c, TCP_ACK, NULL, 0);
        } else if (flags & TCP_RST) {
            c->state = TCP_CLOSED;
        }
        return;
    }

    if (c->state == TCP_SYN_RECEIVED) {
        if (flags & TCP_ACK) {
            c->state = TCP_ESTABLISHED;
            if (l && l->on_connect) {
                l->on_connect(conn_id);
            } else if (l && l->accept_count < ACCEPT_QUEUE_SIZE) {
                /* Accept-queue mode: enqueue the conn_id for net_tcp_accept() */
                l->accept_queue[l->accept_tail] = conn_id;
                l->accept_tail = (l->accept_tail + 1) % ACCEPT_QUEUE_SIZE;
                l->accept_count++;
            } else if (l) {
                /* Accept queue full — reject the connection */
                send_tcp(c, TCP_RST, NULL, 0);
                c->state = TCP_CLOSED;
            }
        }
        return;
    }

    if (c->state == TCP_ESTABLISHED) {
        if (flags & TCP_RST) {
            c->state = TCP_CLOSED;
            c->rx_fin = 1;
            if (l && l->on_close) l->on_close(conn_id);
            return;
        }

        /* ACK processing — congestion control, RTT, SACK, RACK */
        if (flags & TCP_ACK) {
            if (c->tx_unacked_len > 0) {
                /*** NEW ACK ***/
                if ((int32_t)(ack - c->last_ack) > 0) {
                    int acked_some = 0;
                    /* Data delivered by this ACK (for PRR tracking) */
                    uint32_t delivered_data = 0;
                    /* ── RACK: update forward-most delivered marker ──── */
                    /* The ACK covers up to `ack`, so any data below this
                     * is delivered.  rack_fwd_mark tracks the highest
                     * sequence number delivered to the peer. */
                    if ((int32_t)(ack - c->rack_fwd_mark) > 0) {
                        c->rack_fwd_mark  = ack;
                        c->rack_fwd_tick  = timer_get_ticks();
                    }
                    if ((int32_t)(ack - (c->tx_unacked_seq + c->tx_unacked_len)) >= 0) {
                        /* Fully ACKed — clear unacked buffer */
                        acked_some = 1;
                        delivered_data = c->tx_unacked_len;
                        c->tx_unacked_len = 0;
                        c->retrans_count  = 0;
                        c->dupack_count = 0;
                        /* Exit PRR recovery when all pending data is ACKed */
                        if (c->in_recovery) {
                            if (c->cc_algo == 3) {
                                /* NewReno: exit recovery with deflate */
                                newreno_on_full_ack(&c->newreno,
                                                     &c->cwnd,
                                                     c->ssthresh);
                                c->in_recovery = 0;
                            } else {
                                c->in_recovery = 0;
                                c->cwnd = c->ssthresh;
                            }
                        }
                        /* Flush any Nagle-accumulated data now */
                        tcp_flush_nagle(c);
                    } else if ((int32_t)(ack - c->tx_unacked_seq) > 0) {
                        /* Partial ACK */
                        acked_some = 1;
                        uint32_t acked_len = ack - c->tx_unacked_seq;
                        delivered_data = acked_len;
                        uint16_t acked16 = (uint16_t)(acked_len > 65535 ? 65535 : acked_len);
                        c->tx_unacked_len -= acked16;
                        if (c->tx_unacked_len > 0) {
                            memmove(c->tx_unacked_buf, c->tx_unacked_buf + acked16, c->tx_unacked_len);
                            c->tx_unacked_seq = ack;
                        } else {
                            c->tx_unacked_seq = 0;
                            /* Entire buffer ACKed — flush Nagle data now */
                            tcp_flush_nagle(c);
                        }
                        c->retrans_count = 0;
                        c->dupack_count = 0;
                        /* ── NewReno: partial ACK during recovery ─────
                         * If we're in NewReno recovery and this is a
                         * partial ACK (doesn't cover all outstanding),
                         * retransmit the first unacknowledged segment
                         * (RFC 6582 §3.2 Step 3). */
                        if (c->cc_algo == 3 &&
                            c->in_recovery &&
                            c->tx_unacked_len > 0 &&
                            newreno_on_partial_ack(&c->newreno,
                                                    &c->cwnd,
                                                    c->ssthresh)) {
                            /* Retransmit the first unacknowledged segment */
                            uint32_t saved_seq = c->our_seq;
                            c->our_seq = c->tx_unacked_seq;
                            uint16_t chunk = c->tx_unacked_len > 1400
                                             ? 1400 : c->tx_unacked_len;
                            send_tcp(c, TCP_PSH | TCP_ACK,
                                     c->tx_unacked_buf, chunk);
                            c->our_seq += chunk;
                            c->our_seq = saved_seq;
                            c->last_send_tick = timer_get_ticks();
                        }
                    }
                    c->last_ack = ack;

                    if (acked_some) {
                        /* ── PRR handles window during recovery (Item 158) ──
                         * When in fast recovery, PRR controls how much data we
                         * can send.  The standard CC update (slow start / CUBIC
                         * avoidance) is suppressed — PRR keeps the window at
                         * ssthresh and releases data proportionally to ACKs. */
                        if (c->in_recovery) {
                            /* Compute PRR send allowance */
                            uint32_t prr_allow = prr_send_allowed(c, delivered_data);
                            if (prr_allow > 0) {
                                /* Try to flush Nagle data up to PRR allowance.
                                 * tcp_flush_nagle sends data; prr_out is updated
                                 * via the send_tcp path (tracked at call site). */
                                uint16_t old_nagle = c->nagle_buf_len;
                                tcp_flush_nagle(c);
                                uint16_t sent = old_nagle - c->nagle_buf_len;
                                c->prr_out += sent;
                                /* If Nagle had nothing, and we have unacked data
                                 * still waiting, we could retransmit more here.
                                 * For now, PRR paces retransmits via ACK clock. */
                            }
                        } else
                        /* ── Congestion control update (normal, not in recovery) ── */
                        if (c->cc_algo == 1) {
                            /* BBR model-based congestion control */
                            uint64_t now = timer_get_ticks();
                            uint32_t rtt_ticks = (c->srtt > 0) ? (uint32_t)(c->srtt / 8) : 10;
                            if (rtt_ticks < 1) rtt_ticks = 1;
                            /* Use cwnd * 1460 as a proxy for bytes delivered this ACK.
                             * The exact count is tricky because tx_unacked_len is reset
                             * before we reach here for fully-ACKed segments.  BBR's
                             * estimate converges quickly regardless. */
                            uint32_t bbr_bytes = c->cwnd * 1460;
                            bbr_on_ack(&c->bbr, bbr_bytes,
                                       c->cwnd, rtt_ticks, now);
                            /* BBR may set the pacing rate; follow its target cwnd */
                            c->cwnd = bbr_get_cwnd(&c->bbr, c->cwnd);
                            /* BBR paces sends; we need to ensure cwnd respects pacing */
                            uint32_t bbr_pacing = bbr_get_pacing_rate(&c->bbr);
                            if (bbr_pacing > 0 && c->cwnd > bbr_pacing * 2) {
                                /* cwnd capped to pacing_rate * 2 to prevent bursts */
                                uint32_t paced_cwnd = bbr_pacing * 2;
                                if (paced_cwnd < 4) paced_cwnd = 4;
                                if (c->cwnd > paced_cwnd)
                                    c->cwnd = paced_cwnd;
                            }
                        } else if (c->cc_algo == 3) {
                            /* NewReno AIMD congestion control */
                            newreno_on_ack(&c->newreno, &c->cwnd,
                                           c->ssthresh);
                        } else {
                            /* CUBIC congestion control (default) */
                            if (c->cwnd < c->ssthresh) {
                                /* Slow start: exponential growth.
                                 * CUBIC hybrid slow start (RFC 8312 §3): monitor
                                 * ACK spacing and RTT to detect incipient
                                 * congestion and exit slow start before loss. */
                                uint64_t now_ms = timer_get_ms();
                                uint32_t rtt_delta = (uint32_t)(timer_get_ticks() -
                                                                c->last_send_tick);
                                uint32_t rtt_ms = rtt_delta * 10;
                                if (cubic_hystart_update(&c->cubic, rtt_ms,
                                                         now_ms)) {
                                    c->ssthresh = c->cwnd;
                                }
                                c->cwnd++;
                            } else {
                                /* Congestion avoidance: use CUBIC cubic function */
                                uint64_t now = timer_get_ticks();
                                uint32_t rtt_ticks = (c->srtt > 0) ? (uint32_t)(c->srtt / 8) : 10;
                                if (rtt_ticks < 1) rtt_ticks = 1;
                                uint32_t target = cubic_update(&c->cubic, c->cwnd, now, rtt_ticks);
                                /* Aim for the CUBIC target, but ensure at least Reno-equivalent
                                 * growth (1 segment per RTT) for fairness with Reno flows */
                                uint32_t reno_target = c->cwnd + 1;
                                if (target > reno_target)
                                    c->cwnd = target;
                                else
                                    c->cwnd = reno_target;
                            }
                        }
                        if (c->cwnd > 1024) c->cwnd = 1024;

                        /* RTT estimation (Jacobson's algorithm) */
                        int32_t m = (int32_t)(timer_get_ticks() - c->last_send_tick);
                        if (m > 0) {
                            m = (m > 300) ? 300 : m;  /* clamp to 3s */
                            m = m * 8;  /* scale for srtt */
                            if (c->srtt == 0) {
                                c->srtt = m;
                                c->rttvar = m / 2;
                            } else {
                                int32_t delta = m - c->srtt;
                                c->srtt += delta / 8;
                                if (delta < 0) delta = -delta;
                                c->rttvar += (delta - c->rttvar) / 4;
                            }
                            /* RTO = srtt + 4 * rttvar, in ms */
                            int32_t rto_ms = (c->srtt + 4 * c->rttvar) / 8;
                            if (rto_ms < 100) rto_ms = 100;
                            if (rto_ms > 12000) rto_ms = 12000;
                            c->rto = (uint16_t)(rto_ms / 10 + 1);
                            /* ── RACK: track minimum RTT and reordering window ──
                             * rack_min_rtt is the minimum observed RTT in ticks.
                             * rack_reo_wnd = max(min_rtt/4, 1 tick) gives the
                             * reordering margin before declaring loss. */
                            uint32_t rtt_ticks = (uint32_t)m / 8;
                            if (c->rack_min_rtt == 0 || rtt_ticks < c->rack_min_rtt)
                                c->rack_min_rtt = rtt_ticks;
                            uint32_t reo = c->rack_min_rtt / 4;
                            c->rack_reo_wnd = (reo < 1) ? 1 : reo;
                        }
                    }
                }
                /*** DUPLICATE ACK ***/
                else if ((int32_t)(ack - c->last_ack) == 0) {
                    c->dupack_count++;
                    if (c->dupack_count >= 3 && !c->in_recovery) {
                        /* ── PRR Fast Recovery (RFC 6937) — Item 158 ─────────────
                         * On 3 dupacks:
                         * 1. Retransmit only the FIRST missing segment (not all
                         *    unacked data), skipping SACKed blocks.
                         * 2. Initialize PRR tracking state.
                         * 3. Set cwnd = ssthresh (PRR controls subsequent sends).
                         * 4. Record recovery point so we know when to exit.
                         *
                         * Proportional Rate Reduction ensures that the amount of
                         * data sent per ACK during recovery is proportional to the
                         * amount of data delivered, reducing burstiness and
                         * preventing excessive window reductions. */
                        uint32_t saved_seq = c->our_seq;
                        c->our_seq = c->tx_unacked_seq;
                        uint16_t remain = c->tx_unacked_len;
                        const uint8_t *rp = c->tx_unacked_buf;
                        uint16_t first_chunk = 0;

                        /* Skip SACKed blocks to find the first missing segment */
                        while (remain > 0) {
                            uint16_t skip = 0;
                            for (int sb = 0; sb < TCP_MAX_SACK_BLOCKS; sb++) {
                                if (c->sack_blocks[sb].left == 0 &&
                                    c->sack_blocks[sb].right == 0) continue;
                                uint32_t base = c->tx_unacked_seq;
                                if ((int32_t)(base + remain - c->sack_blocks[sb].left) > 0 &&
                                    (int32_t)(base - c->sack_blocks[sb].right) < 0) {
                                    if (c->sack_blocks[sb].left > base &&
                                        c->sack_blocks[sb].left < base + remain) {
                                        uint16_t s = (uint16_t)(c->sack_blocks[sb].left - base);
                                        if (s > skip) skip = s;
                                    }
                                }
                            }
                            if (skip > 0) {
                                c->our_seq += skip;
                                rp += skip;
                                remain -= skip;
                            } else {
                                /* Found the first missing segment — retransmit it */
                                first_chunk = remain > 1400 ? 1400 : remain;
                                send_tcp(c, TCP_PSH | TCP_ACK, rp, first_chunk);
                                c->our_seq += first_chunk;
                                break;  /* Send only one segment per 3 dupacks */
                            }
                        }
                        c->our_seq = saved_seq;

                        /* Record the recovery point (highest seq sent before recovery) */
                        uint32_t prr_recover_seq = c->our_seq;

                        /* Congestion event: handle based on CC algorithm */
                        if (c->cc_algo == 1) {
                            bbr_on_loss(&c->bbr);
                            if (c->ssthresh > c->cwnd / 2)
                                c->ssthresh = c->cwnd / 2;
                            if (c->ssthresh < 2) c->ssthresh = 2;
                            c->cwnd = bbr_get_cwnd(&c->bbr, c->cwnd);
                        } else if (c->cc_algo == 3) {
                            /* NewReno: fast retransmit + fast recovery */
                            newreno_on_3dupacks(&c->newreno, &c->cwnd,
                                                &c->ssthresh, c->our_seq);
                            /* Initialise SACK scoreboard for this recovery
                             * episode (RFC 6675 §4).  Records the starting
                             * outstanding range and enables SACK-based scoring
                             * for loss detection below the 3-dupACK threshold. */
                            newreno_sack_scoreboard_init(&c->newreno,
                                                         c->last_ack,
                                                         c->our_seq);
                        } else {
                            /* CUBIC: handle congestion event */
                            c->ssthresh = cubic_on_loss(&c->cubic, c->cwnd, timer_get_ticks());
                            c->cwnd = c->ssthresh;  /* PRR manages window during recovery */
                        }

                        /* Initialize PRR state — Item 158 */
                        c->in_recovery   = 1;
                        c->prr_delivered = 0;
                        c->prr_out       = first_chunk;

                        c->dupack_count = 0;
                        c->last_send_tick = timer_get_ticks();

                        /* Exit recovery flag: when all pending data is ACKed */
                        (void)prr_recover_seq;  /* used implicitly via tx_unacked_len == 0 */
                    } else if (c->dupack_count >= 3 && c->cc_algo == 3 && c->in_recovery) {
                        /* ── NewReno: additional dupack during recovery ───
                         * Inflate cwnd by 1 MSS per additional dupack
                         * (RFC 6582 §3.2 Step 2). */
                        newreno_on_dup_ack(&c->newreno, &c->cwnd);
                    }
                }
            }
        }

        if (flags & TCP_FIN) {
            c->their_seq = seq + data_len + 1;
            send_tcp(c, TCP_ACK, NULL, 0);
            c->state = TCP_CLOSE_WAIT;
            c->rx_fin = 1;
            if (l && l->on_close) l->on_close(conn_id);
            return;
        }

        if (data_len > 0) {
            uint32_t expected = c->their_seq;
            /* Signed comparisons handle 32-bit sequence number wraparound */
            if ((int32_t)((seq + data_len) - expected) <= 0) {
                /* Duplicate data — send immediate ACK (RFC 5681 §4.2) */
                tcp_flush_delayed_ack(c);
                send_tcp(c, TCP_ACK, NULL, 0);
                return;
            }
            if ((int32_t)(seq - expected) > 0) {
                /* Out-of-order data — send immediate ACK, do not delay */
                tcp_flush_delayed_ack(c);
                send_tcp(c, TCP_ACK, NULL, 0);
                return;
            }
            /* Validate that received data is within our advertised window.
             * We advertise a window of c->their_window bytes starting at c->their_seq.
             * Data outside this window should be silently dropped (RFC 793). */
            uint32_t their_win = c->their_window ? c->their_window : 8192;
            if (seq + data_len > c->their_seq + their_win) {
                kprintf("[TCP] window violation: seq=%u, len=%u, their_seq=%u, window=%u\n",
                        seq, data_len, c->their_seq, their_win);
                return;
            }
            /* Handle TCP urgent pointer (URG flag) — RFC 961 §3.3
             * Record the urgent sequence number so the connection does not
             * desync. Since we do not implement OOB data delivery, we simply
             * note the urgent pointer and allow normal sequence advancement. */
            if (flags & 0x20) {
                c->urg_seq = seq + tcp->urgent;
            }
            uint32_t skip = 0;
            if ((int32_t)(seq - expected) < 0) {
                skip = (uint32_t)(expected - seq);
                if (skip >= data_len) {
                    /* Fully retransmitted — send immediate ACK */
                    tcp_flush_delayed_ack(c);
                    send_tcp(c, TCP_ACK, NULL, 0);
                    return;
                }
                data = (const uint8_t *)data + skip;
                data_len -= (uint16_t)skip;
            }
            c->their_seq = expected + data_len;
            /*
             * Delayed ACK (RFC 1122 §4.2.3.2):
             * Defer the ACK for normal in-order data so it can be piggybacked
             * on the next outgoing segment. Send immediately if PSH is set.
             */
            if (flags & TCP_PSH) {
                /* PSH means the sender wants quick delivery — ACK now */
                tcp_flush_delayed_ack(c);
                send_tcp(c, TCP_ACK, NULL, 0);
            } else {
                /* Schedule a delayed ACK — start the timer */
                c->delayed_ack_pending = 1;
                c->delayed_ack_tick = timer_get_ticks();
                /* If we have a pending Nagle send, flush it now — it carries the ACK */
                if (c->nagle_buf_len > 0)
                    tcp_flush_nagle(c);
            }
            /* Update activity for keepalive */
            c->last_activity_tick = timer_get_ticks();
            if (l && l->on_data) {
                l->on_data(conn_id, data, data_len);
            } else {
                int space = (int)sizeof(c->rxbuf) - c->rxlen;
                int copy = (int)data_len < space ? (int)data_len : space;
                if (copy > 0) {
                    memcpy(c->rxbuf + c->rxlen, data, copy);
                    c->rxlen += copy;
                    /* Wake any poll/select/epoll waiters on this socket */
                    sock_wake_by_conn_id(conn_id);
                }
            }
        }
        return;
    }

    if (c->state == TCP_FIN_WAIT) {
        if (flags & TCP_ACK) {
            c->state = TCP_FIN_WAIT_2;
        }
        if (flags & TCP_FIN) {
            c->their_seq = seq + 1;
            send_tcp(c, TCP_ACK, NULL, 0);
            c->state = TCP_TIME_WAIT;
            c->time_wait_deadline = timer_get_ticks() + TCP_TIME_WAIT_MSL_TICKS;
        }
        return;
    }

    if (c->state == TCP_FIN_WAIT_2) {
        if (flags & TCP_FIN) {
            c->their_seq = seq + 1;
            send_tcp(c, TCP_ACK, NULL, 0);
            c->state = TCP_TIME_WAIT;
            c->time_wait_deadline = timer_get_ticks() + TCP_TIME_WAIT_MSL_TICKS;
        }
        return;
    }

    if (c->state == TCP_TIME_WAIT) {
        /* RFC 1337: send challenge ACK for any segment received in TIME_WAIT.
         * This prevents stale segments from confusing a new connection that
         * reuses the same (src_ip, src_port, dst_ip, dst_port) tuple. */
        send_tcp(c, TCP_ACK, NULL, 0);
        return;
    }

    if (c->state == TCP_CLOSE_WAIT) {
        /* Application should call net_tcp_close to send FIN */
        if (flags & TCP_RST) {
            c->state = TCP_CLOSED;
        }
        return;
    }

    if (c->state == TCP_LAST_ACK) {
        if (flags & TCP_ACK) {
            c->state = TCP_CLOSED;
        }
        return;
    }
}

/* --- Outgoing TCP connect --- */
static uint16_t next_ephemeral_port = 49152;

int net_tcp_connect(uint32_t ip, uint16_t port) {
    int conn_id = alloc_conn();
    if (conn_id < 0) return -1;

    struct tcp_conn *c = &tcp_conns[conn_id];
    c->state = TCP_SYN_SENT;
    c->remote_ip = ip;
    c->remote_port = port;
    /* Pick an ephemeral port, avoiding collisions */
    int port_tries = 0;
    do {
        c->local_port = next_ephemeral_port++;
        if (next_ephemeral_port > 60000) next_ephemeral_port = 49152;
        port_tries++;
        /* Check if port is already in use by any connection */
        int port_in_use = 0;
        for (int i = 0; i < MAX_TCP_CONNS; i++) {
            if (i != conn_id && tcp_conns[i].state != TCP_CLOSED &&
                tcp_conns[i].local_port == c->local_port) {
                port_in_use = 1; break;
            }
        }
        if (!port_in_use) break;
    } while (port_tries < 1000);
    c->our_seq = 10000 + net_ip_id_counter * 1000;
    c->their_seq = 0;
    c->their_window = 0;
    c->rxlen = 0;
    c->rx_fin = 0;
    c->cwnd = 1;
    c->ssthresh = 65535;
    c->dupack_count = 0;
    c->srtt = 0;
    c->rttvar = 0;
    c->tx_unacked_len  = 0;
    c->tx_unacked_seq  = 0;
    c->last_send_tick  = 0;
    c->retrans_count   = 0;
    c->rto             = 30;
    c->tcp_nodelay     = 0;
    c->tcp_cork        = 0;
    c->keepalive       = 0;
    c->keepalive_interval = 500;
    c->keepalive_probes = 0;
    c->keepalive_probes_max = 3;
    c->last_activity_tick = 0;
    c->md5_enabled = 0;
    c->tfo_cookie_present = 0;
    memset(c->md5_digest, 0, sizeof(c->md5_digest));
    memset(c->tfo_cookie, 0, sizeof(c->tfo_cookie));
    memset(c->sack_blocks, 0, sizeof(c->sack_blocks));
    c->sack_pending = 0;

    /* ── TCP Fast Open: use cached cookie if available ─────────────
     * If we have a TFO cookie cached for this server, include it in
     * the SYN so the server can accept data immediately. */
    if (tfo_cache_lookup(ip, c->tfo_cookie)) {
        c->tfo_cookie_present = 1;
    }

    /* Congestion control initialization */
    cubic_init(&c->cubic);
    newreno_init(&c->newreno);
    c->cc_algo = 0;  /* 0 = CUBIC (default), 1 = BBR, 2 = BBRv3, 3 = NewReno */
    /* RACK (Recent ACKnowledgment) loss detection initialization */
    c->rack_fwd_mark    = 0;
    c->rack_fwd_tick    = 0;
    c->rack_reo_wnd     = 1;
    c->rack_min_rtt     = 0;
    /* Nagle / Delayed ACK initialization */
    c->delayed_ack_pending = 0;
    c->nagle_buf_len = 0;

    /* MPTCP initialization */
    c->mptcp_token = 0;
    c->mptcp_rcv_key_valid = 0;
    memset(c->mptcp_snd_key, 0, 8);
    memset(c->mptcp_rcv_key, 0, 8);

    /* PRR (Proportional Rate Reduction) initialization — Item 158 */
    c->in_recovery   = 0;
    c->prr_delivered = 0;
    c->prr_out       = 0;

    send_tcp(c, TCP_SYN, NULL, 0);
    c->our_seq++;

    uint64_t start = timer_get_ticks();
    volatile uint32_t tries = 0;
    while (c->state == TCP_SYN_SENT) {
        net_poll();
        tries++;
        uint64_t now = timer_get_ticks();
        if (now != start && now - start > 500) {
            memset(c, 0, sizeof(*c));
            return -1;
        }
        if (tries > 5000000) {
            memset(c, 0, sizeof(*c));
            return -1;
        }
    }
    if (c->state != TCP_ESTABLISHED) {
        memset(c, 0, sizeof(*c));
        return -1;
    }
    return conn_id;
}

int net_tcp_recv(int conn_id, void *buf, uint16_t bufsize, int timeout_ticks) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return -1;
    struct tcp_conn *c = &tcp_conns[conn_id];

    uint64_t start = timer_get_ticks();
    volatile uint32_t tries = 0;
    while (c->rxlen == 0 && !c->rx_fin) {
        net_poll();
        tries++;
        if (timeout_ticks > 0) {
            uint64_t now = timer_get_ticks();
            if (now != start && (int)(now - start) > timeout_ticks)
                break;
            if (tries > (uint32_t)timeout_ticks * 10000)
                break;
        }
    }
    int got = c->rxlen < bufsize ? c->rxlen : bufsize;
    if (got > 0) {
        memcpy(buf, c->rxbuf, got);
        int remain = c->rxlen - got;
        if (remain > 0)
            memmove(c->rxbuf, c->rxbuf + got, remain);
        c->rxlen = remain;
    }
    return got;
}

void net_tcp_listen(uint16_t port, tcp_connect_handler on_connect,
                    tcp_data_handler on_data, tcp_close_handler on_close) {
    if (net_num_listeners >= MAX_LISTENERS) return;
    struct tcp_listener *l = &net_listeners[net_num_listeners];
    l->port       = port;
    l->on_connect = on_connect;
    l->on_data    = on_data;
    l->on_close   = on_close;
    l->accept_head  = 0;
    l->accept_tail  = 0;
    l->accept_count = 0;
    net_num_listeners++;
}

void net_tcp_unlisten(uint16_t port) {
    for (int i = 0; i < net_num_listeners; i++) {
        if (net_listeners[i].port == port) {
            /* Compact the array */
            for (int j = i; j < net_num_listeners - 1; j++)
                net_listeners[j] = net_listeners[j + 1];
            net_num_listeners--;
            return;
        }
    }
}

int net_tcp_send(int conn_id, const void *data, uint16_t len) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return -1;
    struct tcp_conn *c = &tcp_conns[conn_id];
    if (c->state != TCP_ESTABLISHED) return -1;

    /* Clamp to fit in the retransmit buffer */
    uint16_t send_len = len;
    if (send_len > (uint16_t)sizeof(c->tx_unacked_buf))
        send_len = (uint16_t)sizeof(c->tx_unacked_buf);

    /* ── TCP_CORK: buffer unconditionally until uncorked ────── */
    if (c->tcp_cork) {
        uint16_t space = (uint16_t)sizeof(c->nagle_buf) - c->nagle_buf_len;
        uint16_t copy = send_len < space ? send_len : space;
        if (copy > 0) {
            memcpy(c->nagle_buf + c->nagle_buf_len, data, copy);
            c->nagle_buf_len += copy;
        }
        c->last_activity_tick = timer_get_ticks();
        return 0;
    }

    /* ── TCP_NODELAY or full-MSS: send immediately ──────────── */
    if (c->tcp_nodelay || send_len >= 1400) {
        uint16_t total = send_len;

        /* Merge any accumulated Nagle data with this send */
        if (c->nagle_buf_len > 0) {
            uint16_t merge = (uint16_t)sizeof(c->tx_unacked_buf) - send_len;
            if (merge > c->nagle_buf_len) merge = c->nagle_buf_len;
            /* Build merged buffer: Nagle data first, then new data */
            memcpy(c->tx_unacked_buf, c->nagle_buf, merge);
            memcpy(c->tx_unacked_buf + merge, data, send_len);
            total = send_len + merge;
            c->nagle_buf_len = 0;
        } else {
            memcpy(c->tx_unacked_buf, data, send_len);
        }

        c->tx_unacked_seq = c->our_seq;
        c->tx_unacked_len = total;

        const uint8_t *p = c->tx_unacked_buf;
        uint16_t remaining = total;
        while (remaining > 0) {
            uint16_t chunk = remaining > 1400 ? 1400 : remaining;
            send_tcp(c, TCP_PSH | TCP_ACK, p, chunk);
            c->our_seq += chunk;
            p += chunk;
            remaining -= chunk;
        }

        c->last_send_tick = timer_get_ticks();
        c->retrans_count = 0;
        c->dupack_count = 0;
        c->last_activity_tick = c->last_send_tick;
        if (c->rto == 0) c->rto = 30;
        return 0;
    }

    /* ── Nagle algorithm (RFC 896) ───────────────────────────
     *
     * If there is unacknowledged data outstanding, delay sending
     * small segments (< MSS).  Accumulate into the tx_unacked_buf
     * right after the outstanding data and send when either:
     *   a) the ACK for the outstanding data arrives (tcp_flush_nagle)
     *   b) a subsequent send with full-MSS or NODELAY flushes it
     *   c) the caller sets TCP_CORK and later clears it
     */
    if (c->tx_unacked_len > 0) {
        /* Accumulate into the Nagle buffer for later merging */
        uint16_t space = (uint16_t)sizeof(c->nagle_buf) - c->nagle_buf_len;
        uint16_t copy = send_len < space ? send_len : space;
        if (copy > 0) {
            memcpy(c->nagle_buf + c->nagle_buf_len, data, copy);
            c->nagle_buf_len += copy;
        }
        c->last_activity_tick = timer_get_ticks();
        return 0;
    }

    /* ── No outstanding data — send immediately ────────────── */
    c->tx_unacked_seq = c->our_seq;
    memcpy(c->tx_unacked_buf, data, send_len);
    c->tx_unacked_len = send_len;

    const uint8_t *p = (const uint8_t *)data;
    uint16_t remaining = send_len;
    while (remaining > 0) {
        uint16_t chunk = remaining > 1400 ? 1400 : remaining;
        send_tcp(c, TCP_PSH | TCP_ACK, p, chunk);
        c->our_seq += chunk;
        p += chunk;
        remaining -= chunk;
    }

    c->last_send_tick = timer_get_ticks();
    c->retrans_count = 0;
    c->dupack_count = 0;
    c->last_activity_tick = c->last_send_tick;
    if (c->rto == 0) c->rto = 30;
    return 0;
}

/* ── Nagle / Delayed ACK helpers ────────────────────────────────── */

/*
 * Send (or flush) a pending delayed ACK.
 * Called when we need to stop delaying and send the ACK now.
 */
static void tcp_flush_delayed_ack(struct tcp_conn *c) {
    if (c->delayed_ack_pending) {
        c->delayed_ack_pending = 0;
        send_tcp(c, TCP_ACK, NULL, 0);
    }
}

/*
 * Flush the Nagle buffer -- send accumulated small writes.
 * Called when the previous outstanding data has been ACKed and we
 * have data waiting in the buffer.
 */
static void tcp_flush_nagle(struct tcp_conn *c) {
    if (c->nagle_buf_len == 0) return;

    uint16_t remaining = c->nagle_buf_len;
    const uint8_t *p = c->nagle_buf;
    c->tx_unacked_seq = c->our_seq;

    while (remaining > 0) {
        uint16_t chunk = remaining > 1400 ? 1400 : remaining;
        /* Piggyback any pending delayed ACK on the data segment */
        uint8_t flags = TCP_PSH | TCP_ACK;
        send_tcp(c, flags, p, chunk);
        c->our_seq += chunk;
        p += chunk;
        remaining -= chunk;
    }

    /* Data is now outstanding — copy to retransmission buffer */
    memcpy(c->tx_unacked_buf, c->nagle_buf, c->nagle_buf_len);
    c->tx_unacked_len = c->nagle_buf_len;
    c->nagle_buf_len = 0;
    c->last_send_tick = timer_get_ticks();
    c->retrans_count = 0;
    c->last_activity_tick = c->last_send_tick;
}

void net_tcp_close(int conn_id) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return;
    struct tcp_conn *c = &tcp_conns[conn_id];

    /* Flush any pending delayed ACK and Nagle data before closing */
    tcp_flush_delayed_ack(c);
    if (c->nagle_buf_len > 0)
        tcp_flush_nagle(c);

    switch (c->state) {
        case TCP_ESTABLISHED:
            send_tcp(c, TCP_FIN | TCP_ACK, NULL, 0);
            c->our_seq++;
            c->state = TCP_FIN_WAIT;
            break;
        case TCP_CLOSE_WAIT:
            send_tcp(c, TCP_FIN | TCP_ACK, NULL, 0);
            c->our_seq++;
            c->state = TCP_LAST_ACK;
            break;
        case TCP_SYN_SENT:
            /* fallthrough */
        case TCP_SYN_RECEIVED:
            send_tcp(c, TCP_RST, NULL, 0);
            c->state = TCP_CLOSED;
            break;
        case TCP_FIN_WAIT:
            /* fallthrough */
        case TCP_FIN_WAIT_2:
            /* fallthrough */
        case TCP_CLOSING:
            /* fallthrough */
        case TCP_LAST_ACK:
            /* fallthrough */
        case TCP_CLOSED:
        default:
            break;
    }
}

/* --- Blocking server accept --- */

int net_tcp_accept(uint16_t port, int timeout_ticks) {
    struct tcp_listener *l = find_listener(port);
    if (!l) return -1;

    uint64_t start = timer_get_ticks();
    while (l->accept_count == 0) {
        net_poll();
        scheduler_yield();  /* allow other processes to run while waiting */
        if (timeout_ticks > 0) {
            uint64_t now = timer_get_ticks();
            if (now != start && (int)(now - start) > timeout_ticks)
                return -1;
        }
    }
    /* Guard against accept_count being out of sync (should never happen) */
    if (l->accept_count <= 0)
        return -1;
    int conn_id = l->accept_queue[l->accept_head];
    /* Validate conn_id — must be a valid established connection */
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS ||
        tcp_conns[conn_id].state == TCP_CLOSED)
        return -1;
    l->accept_head = (l->accept_head + 1) % ACCEPT_QUEUE_SIZE;
    l->accept_count--;
    return conn_id;
}

void net_conn_list(void (*cb)(uint16_t lport, uint32_t rip, uint16_t rport, int state)) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].state != TCP_CLOSED)
            cb(tcp_conns[i].local_port, tcp_conns[i].remote_ip,
               tcp_conns[i].remote_port, (int)tcp_conns[i].state);
    }
}

/* Periodic retransmission check — called from timer every N ticks */
void net_tcp_check_retransmit(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        struct tcp_conn *c = &tcp_conns[i];

        /* Clean up TIME_WAIT connections after 2*MSL (60 seconds at 100Hz) */
        if (c->state == TCP_TIME_WAIT) {
            if (now >= c->time_wait_deadline)
                c->state = TCP_CLOSED;
            continue;
        }

        /* Skip connections that are not established */
        if (c->state != TCP_ESTABLISHED) continue;

        /*
         * Delayed ACK timeout (RFC 1122 §4.2.3.2):
         * Send a pure ACK if we've been delaying for more than ~20ms (2 ticks).
         * This bounds the acknowledgment delay so the sender doesn't stall.
         */
        if (c->delayed_ack_pending && (now - c->delayed_ack_tick >= 2)) {
            c->delayed_ack_pending = 0;
            send_tcp(c, TCP_ACK, NULL, 0);
        }

        /*
         * Nagle accumulation timeout:
         * If we have accumulated data in the Nagle buffer but the
         * outstanding data hasn't been ACKed for a while, send the
         * accumulated data anyway (up to ~50ms = 5 ticks).
         * This prevents starvation when the peer is slow to ACK.
         */
        if (c->nagle_buf_len > 0 && c->tx_unacked_len > 0 &&
            (now - c->last_send_tick >= 5)) {
            /* Merge Nagle buffer with outstanding data and send */
            uint16_t total = c->tx_unacked_len + c->nagle_buf_len;
            if (total > sizeof(c->tx_unacked_buf))
                total = sizeof(c->tx_unacked_buf);
            uint16_t merge = total - c->tx_unacked_len;
            if (merge > c->nagle_buf_len) merge = c->nagle_buf_len;
            memcpy(c->tx_unacked_buf + c->tx_unacked_len, c->nagle_buf, merge);
            c->nagle_buf_len -= merge;
            if (c->nagle_buf_len > 0)
                memmove(c->nagle_buf, c->nagle_buf + merge, c->nagle_buf_len);

            uint32_t saved_seq = c->our_seq;
            c->our_seq = c->tx_unacked_seq;
            const uint8_t *rp = c->tx_unacked_buf;
            uint16_t remain = c->tx_unacked_len + merge;
            while (remain > 0) {
                uint16_t chunk = remain > 1400 ? 1400 : remain;
                send_tcp(c, TCP_PSH | TCP_ACK, rp, chunk);
                c->our_seq += chunk;
                rp += chunk;
                remain -= chunk;
            }
            c->tx_unacked_len = c->tx_unacked_len + merge;
            c->our_seq = saved_seq + merge; /* advance past what we just sent */
        }

        /* ── RACK loss detection ────────────────────────────────
         * RACK (Recent ACKnowledgment) uses time-based detection:
         * if the forward-most delivered sequence number has advanced
         * past our unacked data AND enough time (reo_wnd + min_rtt)
         * has elapsed since we sent it, we declare it lost — even
         * before the 3-duplicate-ACK or RTO threshold.
         *
         * This catches single-packet losses much faster than the
         * classic dupack-count approach. */
        if (c->tx_unacked_len > 0 && c->rack_fwd_mark > 0 &&
            c->sack_pending &&
            (int32_t)(c->rack_fwd_mark - (c->tx_unacked_seq + c->tx_unacked_len)) > 0)
        {
            uint64_t elapsed = now - c->last_send_tick;
            uint32_t rack_thresh = c->rack_reo_wnd + c->rack_min_rtt;
            if (rack_thresh > 0 && elapsed >= rack_thresh &&
                elapsed < (uint64_t)c->rto) /* don't dupe RTO retransmit */
            {
                /* ── RACK-triggered fast retransmit ── */
                uint32_t saved_seq = c->our_seq;
                c->our_seq = c->tx_unacked_seq;
                uint16_t remain = c->tx_unacked_len;
                const uint8_t *rp = c->tx_unacked_buf;
                while (remain > 0) {
                    uint16_t skip = 0;
                    for (int sb = 0; sb < TCP_MAX_SACK_BLOCKS; sb++) {
                        if (c->sack_blocks[sb].left == 0 && c->sack_blocks[sb].right == 0)
                            continue;
                        uint32_t base = c->tx_unacked_seq;
                        if ((int32_t)(base + remain - c->sack_blocks[sb].left) > 0 &&
                            (int32_t)(base - c->sack_blocks[sb].right) < 0) {
                            if (c->sack_blocks[sb].left > base &&
                                c->sack_blocks[sb].left < base + remain) {
                                uint16_t s = (uint16_t)(c->sack_blocks[sb].left - base);
                                if (s > skip) skip = s;
                            }
                        }
                    }
                    if (skip > 0) {
                        c->our_seq += skip;
                        rp += skip;
                        remain -= skip;
                    } else {
                        uint16_t chunk = remain > 1400 ? 1400 : remain;
                        send_tcp(c, TCP_PSH | TCP_ACK, rp, chunk);
                        c->our_seq += chunk;
                        rp += chunk;
                        remain -= chunk;
                    }
                }
                c->our_seq = saved_seq;
                /* Congestion event: handle based on CC algorithm */
                if (c->cc_algo == 3) {
                    /* NewReno: fast retransmit */
                    newreno_on_3dupacks(&c->newreno, &c->cwnd,
                                        &c->ssthresh, c->our_seq);
                    c->in_recovery = 1;
                    c->prr_delivered = 0;
                    c->prr_out = 0;
                } else {
                    /* CUBIC congestion event: handle */
                    c->ssthresh = cubic_on_loss(&c->cubic, c->cwnd, timer_get_ticks());
                    c->cwnd = c->ssthresh + 3;
                }
                c->dupack_count = 0;
                c->last_send_tick = timer_get_ticks();
                /* Advance RACK state — we retransmitted the earliest data,
                 * so shift fwd_mark forward by the retransmitted amount.
                 * Prevent immediate re-triggering. */
                c->rack_fwd_mark = c->tx_unacked_seq + c->tx_unacked_len;
                c->rack_fwd_tick = timer_get_ticks();
                continue;
            }
        }

        /* ── RTO-based retransmission ────────────────────────── */
        if (c->tx_unacked_len == 0) continue;
        if (now - c->last_send_tick < c->rto) continue;

        /* Give up after 5 retransmissions (RTO would be ~32 s at that point) */
        if (c->retrans_count >= 5) {
            c->state = TCP_CLOSED;
            c->rx_fin = 1;
            c->tx_unacked_len = 0;
            continue;
        }

        /* Retransmit using the saved sequence number, in MSS-sized chunks,
         * skipping data already reported as received via SACK. */
        uint32_t saved_seq = c->our_seq;
        c->our_seq = c->tx_unacked_seq;
        uint16_t remain = c->tx_unacked_len;
        const uint8_t *rp = c->tx_unacked_buf;

        /* Build a list of SACK-covered byte ranges to skip */
        while (remain > 0) {
            /* Determine how many bytes to skip based on SACK blocks */
            uint16_t skip = 0;
            for (int sb = 0; sb < TCP_MAX_SACK_BLOCKS; sb++) {
                if (c->sack_blocks[sb].left == 0 && c->sack_blocks[sb].right == 0)
                    continue;
                uint32_t seq_off = c->tx_unacked_seq;
                if ((int32_t)(seq_off + remain - c->sack_blocks[sb].left) > 0 &&
                    (int32_t)(seq_off - c->sack_blocks[sb].right) < 0) {
                    if (c->sack_blocks[sb].left > seq_off &&
                        c->sack_blocks[sb].left < seq_off + remain) {
                        uint16_t s = (uint16_t)(c->sack_blocks[sb].left - seq_off);
                        if (s > skip) skip = s;
                    }
                }
            }
            if (skip > 0) {
                c->our_seq += skip;
                rp += skip;
                remain -= skip;
            } else {
                uint16_t chunk = remain > 1400 ? 1400 : remain;
                send_tcp(c, TCP_PSH | TCP_ACK, rp, chunk);
                c->our_seq += chunk;
                rp += chunk;
                remain -= chunk;
            }
        }
        c->our_seq = saved_seq;

        c->last_send_tick = now;
        c->retrans_count++;
        /* Exponential back-off, cap at 64 s */
        c->rto = (c->rto * 2 > 6400) ? 6400 : (uint16_t)(c->rto * 2);
        /* Exit PRR recovery on RTO — timeout is more severe than fast recovery */
        c->in_recovery = 0;
        newreno_abort_recovery(&c->newreno);
        if (c->cc_algo == 3) {
            /* NewReno: undo any partial ACK tracking, standard RTO action */
            c->ssthresh = c->cwnd / 2;
            if (c->ssthresh < 2) c->ssthresh = 2;
        } else {
            /* CUBIC congestion control: handle RTO timeout event */
            c->ssthresh = cubic_on_loss(&c->cubic, c->cwnd, now);
            if (c->ssthresh < 2) c->ssthresh = 2;
        }
        c->cwnd = 1;
    }
}

/* ── TCP auto-tuning ────────────────────────────────────────────── */

/* Auto-size the receive window based on a simple BDP estimate.
 * Called periodically (from net_tcp_check_retransmit) for established
 * connections.  Starts at 64KB, rises to 256KB on good connection,
 * decreases on loss (retransmission or dupack).
 */
#define TCP_MIN_RCV_WND  65536   /* start at 64KB */
#define TCP_MAX_RCV_WND  262144  /* max 256KB */

void net_tcp_auto_tune_rcv_wnd(struct tcp_conn *c)
{
    if (c->state != TCP_ESTABLISHED) return;

    /* Estimate RTT in ticks — use min_rtt or a fallback of 10 ticks (~100ms) */
    uint32_t rtt = c->rack_min_rtt > 0 ? c->rack_min_rtt : 10;
    if (rtt < 1) rtt = 1;

    /* Base window on cwnd: start at 64KB, scale with cwnd growth.
     * On good connection (no loss, high cwnd), window grows toward 256KB.
     * On loss (retrans_count > 0 or dupack_count > 0), shrink. */
    uint32_t target;
    if (c->retrans_count > 0 || c->dupack_count > 0) {
        /* Loss detected — shrink window */
        target = c->their_window / 2;
        if (target < TCP_MIN_RCV_WND / 2)
            target = TCP_MIN_RCV_WND / 2;
    } else {
        /* Normal operation — scale with cwnd */
        target = c->cwnd * 1460;  /* cwnd in segments × MSS */
        if (target > TCP_MAX_RCV_WND)
            target = TCP_MAX_RCV_WND;
    }

    /* Ensure minimum */
    if (target < TCP_MIN_RCV_WND)
        target = TCP_MIN_RCV_WND;

    /* Clamp to uint16_t for TCP header */
    if (target > 65535)
        target = 65535;
    c->their_window = (uint16_t)target;
}

/* Slow start after idle: if a connection has been idle for more than
 * one retransmission timeout, reduce cwnd back to the initial window
 * (typically 10 segments per RFC 6928) to avoid bursting.
 * This is called before sending data on an idle connection.
 */
void net_tcp_slow_start_after_idle(struct tcp_conn *c)
{
    if (!c || c->state != TCP_ESTABLISHED) return;

    uint64_t now = timer_get_ticks();
    uint64_t idle_ticks = now - c->last_send_tick;

    /* If idle for more than 1 RTO, reset cwnd */
    if (idle_ticks > (uint64_t)c->rto && c->rto > 0) {
        if (c->cwnd > 10) {
            /* Record W_max for CUBIC before reducing */
            if (c->cubic.use_cubic) {
                c->cubic.wmax = c->cwnd;
            }
            /* Reset NewReno AIMD counter on idle */
            if (c->cc_algo == 3) {
                c->newreno.reno_ack_count = 0;
            }
            c->ssthresh = c->cwnd / 2;
            if (c->ssthresh < 2) c->ssthresh = 2;
            c->cwnd = 10; /* initial window per RFC 6928 */
            c->dupack_count = 0;
        }
    }
}

/* ── Call the auto-tuning for all established connections ───────── */
void net_tcp_auto_tune_all(void)
{
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        struct tcp_conn *c = &tcp_conns[i];
        if (c->state != TCP_ESTABLISHED) continue;
        net_tcp_auto_tune_rcv_wnd(c);
        net_tcp_slow_start_after_idle(c);
    }
}

/* Keepalive check ──────────────────────────────────────────── */

#define KEEPALIVE_INTERVAL_DEFAULT 500
#define KEEPALIVE_PROBES_MAX_DEFAULT 3

void net_tcp_set_keepalive(int conn_id, int keepalive) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return;
    struct tcp_conn *c = &tcp_conns[conn_id];
    c->keepalive = keepalive;
    if (keepalive) {
        c->keepalive_interval = KEEPALIVE_INTERVAL_DEFAULT;
        c->keepalive_probes = 0;
        c->keepalive_probes_max = KEEPALIVE_PROBES_MAX_DEFAULT;
        c->last_activity_tick = timer_get_ticks();
    }
}

int net_tcp_get_keepalive(int conn_id) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return 0;
    return tcp_conns[conn_id].keepalive;
}

void net_tcp_check_keepalive(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        struct tcp_conn *c = &tcp_conns[i];
        if (c->state != TCP_ESTABLISHED || !c->keepalive) continue;
        if (now - c->last_activity_tick >= c->keepalive_interval) {
            if (c->keepalive_probes >= c->keepalive_probes_max) {
                c->state = TCP_CLOSED;
                c->rx_fin = 1;
                continue;
            }
            uint32_t saved_seq = c->our_seq;
            c->our_seq = c->tx_unacked_seq > 0 ?
                         c->tx_unacked_seq - 1 : c->our_seq - 1;
            send_tcp(c, TCP_ACK, NULL, 0);
            c->our_seq = saved_seq;
            c->keepalive_probes++;
            c->last_activity_tick = now;
        }
    }
}

int net_tcp_get_info(int conn_id, struct tcp_conn_info *info) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return -1;
    struct tcp_conn *c = &tcp_conns[conn_id];
    if (c->state == TCP_CLOSED) return -1;
    info->local_port = c->local_port;
    info->remote_ip = c->remote_ip;
    info->remote_port = c->remote_port;
    info->state = (int)c->state;
    info->cwnd = c->cwnd;
    info->ssthresh = c->ssthresh;
    info->last_send_tick = c->last_send_tick;
    info->retrans_count = c->retrans_count;
    return 0;
}

/* Return number of bytes available in the TCP receive buffer */
int net_tcp_available(int conn_id)
{
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return 0;
    struct tcp_conn *c = &tcp_conns[conn_id];
    if (c->state == TCP_CLOSED) return 0;
    return c->rxlen;
}

/* Return 1 if the TCP connection is in ESTABLISHED state (writable) */
int net_tcp_is_connected(int conn_id)
{
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return 0;
    return (tcp_conns[conn_id].state == TCP_ESTABLISHED) ? 1 : 0;
}

/* Return 1 if the TCP connection has received FIN or is closed */
int net_tcp_has_closed(int conn_id)
{
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return 1; /* invalid = closed */
    struct tcp_conn *c = &tcp_conns[conn_id];
    return (c->state == TCP_CLOSED || c->rx_fin) ? 1 : 0;
}

/* ── Implement: tcp_open ──────────────────────────────── */
int tcp_open(void *sk)
{
    if (!sk) return -EINVAL;
    kprintf("[tcp] tcp_open: allocating TCP connection\n");
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].state == TCP_CLOSED) {
            memset(&tcp_conns[i], 0, sizeof(struct tcp_conn));
            tcp_conns[i].state = TCP_CLOSED;
            tcp_conns[i].cwnd = 10;
            tcp_conns[i].ssthresh = 0x7FFFFFFF;
            tcp_conns[i].rto = 30;
            tcp_conns[i].keepalive_interval = 500;
            tcp_conns[i].keepalive_probes_max = 3;
            return i;
        }
    }
    return -ENOMEM;
}
/* ── Implement: tcp_close ─────────────────────────────── */
int tcp_close(void *sk)
{
    if (!sk) return -EINVAL;
    int conn_id = *(int *)sk;
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return -EINVAL;
    if (tcp_conns[conn_id].state != TCP_CLOSED) {
        net_tcp_close(conn_id);
    }
    return 0;
}
/* ── Implement: tcp_connect ───────────────────────────── */
int tcp_connect(void *sk, void *addr)
{
    if (!sk || !addr) return -EINVAL;
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    int conn_id = net_tcp_connect(sin->sin_addr.s_addr, ntohs(sin->sin_port));
    if (conn_id < 0) return -ECONNREFUSED;
    *(int *)sk = conn_id;
    return 0;
}
/* ── Implement: tcp_disconnect ────────────────────────── */
int tcp_disconnect(void *sk)
{
    if (!sk) return -EINVAL;
    int conn_id = *(int *)sk;
    if (conn_id >= 0 && conn_id < MAX_TCP_CONNS) {
        if (tcp_conns[conn_id].state != TCP_CLOSED) {
            net_tcp_close(conn_id);
        }
        tcp_conns[conn_id].state = TCP_CLOSED;
    }
    return 0;
}
/* ── Implement: tcp_sendmsg ───────────────────────────── */
int tcp_sendmsg(void *sk, void *msg, size_t len)
{
    if (!sk || !msg) return -EINVAL;
    int conn_id = *(int *)sk;
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return -EINVAL;
    if (tcp_conns[conn_id].state != TCP_ESTABLISHED) return -ENOTCONN;
    uint16_t send_len = (uint16_t)(len > 65535 ? 65535 : len);
    return net_tcp_send(conn_id, msg, send_len);
}
/* ── Implement: tcp_recvmsg ───────────────────────────── */
int tcp_recvmsg(void *sk, void *msg, size_t len)
{
    if (!sk || !msg) return -EINVAL;
    int conn_id = *(int *)sk;
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return -EINVAL;
    uint16_t recv_len = (uint16_t)(len > 65535 ? 65535 : len);
    return net_tcp_recv(conn_id, msg, recv_len, 10);
}
