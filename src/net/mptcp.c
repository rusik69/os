/* mptcp.c — Multipath TCP (RFC 8684) subflow management */

#include "mptcp.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"
#include "timer.h"
#include "sha256.h"
#include "hmac.h"

#define MPTCP_MAX_CONNS 8

static struct mptcp_conn mptcp_conns[MPTCP_MAX_CONNS];
static spinlock_t mptcp_lock;
static int mptcp_initialized = 0;
static uint32_t mptcp_next_token = 0x10000000;

void mptcp_init(void)
{
    if (mptcp_initialized) return;
    spinlock_init(&mptcp_lock);
    memset(mptcp_conns, 0, sizeof(mptcp_conns));
    mptcp_initialized = 1;
    /* Seed token generation */
    mptcp_next_token = 0x10000000 + (uint32_t)(timer_get_ticks() & 0x0FFFFFFF);
    kprintf("[OK] MPTCP: Multipath TCP subflow management initialized\n");
}

static int mptcp_find_free(void)
{
    for (int i = 0; i < MPTCP_MAX_CONNS; i++) {
        if (!mptcp_conns[i].used)
            return i;
    }
    return -1;
}

static struct mptcp_conn *mptcp_find_by_token(uint32_t token)
{
    for (int i = 0; i < MPTCP_MAX_CONNS; i++) {
        if (mptcp_conns[i].used && mptcp_conns[i].token == token)
            return &mptcp_conns[i];
    }
    return NULL;
}

int mptcp_create(void)
{
    spinlock_acquire(&mptcp_lock);
    int slot = mptcp_find_free();
    if (slot < 0) {
        spinlock_release(&mptcp_lock);
        return -ENOMEM;
    }

    struct mptcp_conn *mc = &mptcp_conns[slot];
    memset(mc, 0, sizeof(*mc));
    mc->used = 1;
    mc->token = mptcp_next_token++;
    mc->snd_data_seq = 0;
    mc->snd_data_ack = 0;
    mc->rcv_data_seq = 0;
    mc->rcv_data_ack = 0;

    /* Generate random 64-bit key using RNG */
    uint64_t key = 0;
    /* Use mptcp_next_token + ticks as seed */
    key = (uint64_t)mptcp_next_token;
    key |= ((uint64_t)timer_get_ticks()) << 32;
    mc->snd_key[0] = (uint8_t)(key & 0xFF);
    mc->snd_key[1] = (uint8_t)((key >> 8) & 0xFF);
    mc->snd_key[2] = (uint8_t)((key >> 16) & 0xFF);
    mc->snd_key[3] = (uint8_t)((key >> 24) & 0xFF);
    mc->snd_key[4] = (uint8_t)((key >> 32) & 0xFF);
    mc->snd_key[5] = (uint8_t)((key >> 40) & 0xFF);
    mc->snd_key[6] = (uint8_t)((key >> 48) & 0xFF);
    mc->snd_key[7] = (uint8_t)((key >> 56) & 0xFF);

    spinlock_release(&mptcp_lock);
    return (int)mc->token;
}

int mptcp_add_subflow(uint32_t token, int conn_id, uint32_t addr, uint16_t port)
{
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc || mc->num_subflows >= MPTCP_MAX_SUBFLOWS) {
        spinlock_release(&mptcp_lock);
        return -EINVAL;
    }

    struct mptcp_subflow *sf = &mc->subflows[mc->num_subflows++];
    sf->used = 1;
    sf->conn_id = conn_id;
    sf->token = token;
    sf->snd_isn = 0;
    sf->rcv_isn = 0;
    memcpy(sf->key, mc->snd_key, 8);
    /* Initialize DSS tracking fields */
    sf->dss_data_seq = 0;
    sf->dss_subflow_seq = 0;
    sf->dss_mapped_len = 0;

    kprintf("[MPTCP] Added subflow conn_id=%d to token %u\n", conn_id, token);
    spinlock_release(&mptcp_lock);
    return 0;
}

int mptcp_remove_subflow(uint32_t token, uint32_t addr, uint16_t port)
{
    (void)addr;
    (void)port;
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        return -EINVAL;
    }

    for (int i = 0; i < mc->num_subflows; i++) {
        if (mc->subflows[i].used) {
            /* Remove this subflow */
            mc->subflows[i].used = 0;
            for (int j = i; j < mc->num_subflows - 1; j++)
                mc->subflows[j] = mc->subflows[j + 1];
            mc->num_subflows--;
            spinlock_release(&mptcp_lock);
            return 0;
        }
    }
    spinlock_release(&mptcp_lock);
    return -ENOENT;
}

int mptcp_send(uint32_t token, const void *data, uint32_t len)
{
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc || !mc->established) {
        spinlock_release(&mptcp_lock);
        return -EINVAL;
    }

    /* Find first active subflow to send on */
    for (int i = 0; i < mc->num_subflows; i++) {
        if (mc->subflows[i].used && !mc->subflows[i].backup) {
            /* Send on this subflow — record DSS mapping so the TCP
             * stack can embed the DSS option on data segments. */
            struct mptcp_subflow *sf = &mc->subflows[i];
            sf->dss_data_seq = mc->snd_data_seq;
            sf->dss_subflow_seq = sf->snd_isn;  /* subflow initial seq number */
            sf->dss_mapped_len = (uint16_t)len;

            /* Advance data-level sequence number */
            mc->snd_data_seq += len;

            spinlock_release(&mptcp_lock);
            return len;
        }
    }
    spinlock_release(&mptcp_lock);
    return -ENETDOWN;
}

int mptcp_recv(uint32_t token, void *buf, uint32_t maxlen)
{
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        return -EINVAL;
    }
    if (mc->rcvlen == 0) {
        spinlock_release(&mptcp_lock);
        return -EAGAIN;
    }

    uint32_t copy_len = (mc->rcvlen < maxlen) ? mc->rcvlen : maxlen;
    memcpy(buf, mc->rcvbuf, copy_len);
    mc->rcvlen = 0;
    spinlock_release(&mptcp_lock);
    return copy_len;
}

void mptcp_close(uint32_t token)
{
    spinlock_acquire(&mptcp_lock);
    for (int i = 0; i < MPTCP_MAX_CONNS; i++) {
        if (mptcp_conns[i].used && mptcp_conns[i].token == token) {
            memset(&mptcp_conns[i], 0, sizeof(struct mptcp_conn));
            break;
        }
    }
    spinlock_release(&mptcp_lock);
}

int mptcp_get_token(void)
{
    return mptcp_create();
}

/* ── mptcp_associate: Link a TCP connection with MPTCP metadata ────
 * Looks up the MPTCP connection by token and copies its sender key
 * into the tcp_conn structure so the TCP stack can include MP_CAPABLE
 * options on SYN/SYN-ACK. */
int mptcp_associate(int conn_id, uint32_t token)
{
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) {
        kprintf("[mptcp] mptcp_associate: invalid conn_id %d\n", conn_id);
        return -EINVAL;
    }
    struct tcp_conn *c = &tcp_conns[conn_id];
    if (c->state == TCP_CLOSED) {
        kprintf("[mptcp] mptcp_associate: conn %d is closed\n", conn_id);
        return -ECONNRESET;
    }

    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_associate: token %u not found\n", token);
        return -EINVAL;
    }
    c->mptcp_token = token;
    memcpy(c->mptcp_snd_key, mc->snd_key, 8);
    c->mptcp_rcv_key_valid = 0;
    memset(c->mptcp_rcv_key, 0, 8);
    spinlock_release(&mptcp_lock);
    return 0;
}

/* ── mptcp_build_capable_syn ──────────────────────────────────────
 * Build MP_CAPABLE option buffer for an initial SYN (client side).
 * Format: kind=30, len=12, subtype=0, flags=0, reserved=0, key(8).
 * Returns bytes written to buf, or negative on error. */
int mptcp_build_capable_syn(uint8_t *buf, uint16_t *len, const uint8_t snd_key[8])
{
    if (!buf || !len || !snd_key)
        return -EINVAL;
    if (*len < MPTCP_CAPABLE_SYN_LEN)
        return -ENOSPC;

    buf[0] = TCPOPT_MPTCP;        /* kind = 30 */
    buf[1] = MPTCP_CAPABLE_SYN_LEN; /* length = 12 */
    buf[2] = (MPTCP_CAPABLE << 4) | 0x00; /* subtype=0, flags=0 */
    buf[3] = 0;                     /* reserved */
    memcpy(buf + 4, snd_key, 8);
    *len = MPTCP_CAPABLE_SYN_LEN;
    return 0;
}

/* ── mptcp_build_capable_synack ────────────────────────────────────
 * Build MP_CAPABLE option buffer for a SYN+ACK (server side).
 * Same wire format as the SYN version: sender's key only (no HMAC). */
int mptcp_build_capable_synack(uint8_t *buf, uint16_t *len, const uint8_t snd_key[8])
{
    /* Wire format is identical to capable_syn for v1 (no HMAC) */
    return mptcp_build_capable_syn(buf, len, snd_key);
}

/* ── mptcp_build_capable_ack ──────────────────────────────────────
 * Build MP_CAPABLE option buffer for the 3rd ACK (completing handshake).
 * Format: kind=30, len=24, subtype=0, flags=1, key(8), peer_key(8),
 *         HMAC-SHA256(snd_key || rcv_key, "mptcp-capable-hmac")[0:8]. */
int mptcp_build_capable_ack(uint8_t *buf, uint16_t *len,
                             const uint8_t snd_key[8], const uint8_t rcv_key[8])
{
    if (!buf || !len || !snd_key || !rcv_key)
        return -EINVAL;
    if (*len < MPTCP_CAPABLE_ACK_LEN)
        return -ENOSPC;

    buf[0] = TCPOPT_MPTCP;          /* kind = 30 */
    buf[1] = MPTCP_CAPABLE_ACK_LEN; /* length = 24 */
    buf[2] = (MPTCP_CAPABLE << 4) | MPTCP_CAPABLE_HMAC; /* subtype=0, H=1 */
    buf[3] = 0;                     /* reserved */
    memcpy(buf + 4, snd_key, 8);       /* sender's key */
    memcpy(buf + 12, rcv_key, 8);      /* receiver's key */

    /* Compute HMAC-SHA256(snd_key || rcv_key, "mptcp-capable-hmac")[0:8] */
    {
        uint8_t hmac_key[16];
        uint8_t hmac_full[HMAC_SHA256_DIGEST_SIZE];
        memcpy(hmac_key, snd_key, 8);
        memcpy(hmac_key + 8, rcv_key, 8);
        hmac_sha256(hmac_key, 16,
                    (const uint8_t *)"mptcp-capable-hmac", 19,
                    hmac_full);
        memcpy(buf + 20, hmac_full, 8); /* truncated HMAC: first 8 bytes */
    }

    *len = MPTCP_CAPABLE_ACK_LEN;
    return 0;
}

/* ── mptcp_parse_capable ──────────────────────────────────────────
 * Parse an MP_CAPABLE option and extract the peer's (sender's) key.
 * Returns 0 on success, negative on error.  On success, peer_key
 * is filled with the 8-byte key from the option. */
int mptcp_parse_capable(const uint8_t *opt, uint16_t optlen, uint8_t peer_key[8])
{
    if (!opt || !peer_key)
        return -EINVAL;
    if (optlen < MPTCP_CAPABLE_SYN_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP)
        return -EINVAL;
    if (opt[2] >> 4 != MPTCP_CAPABLE)
        return -EINVAL;

    /* Sender's key is at offset 4 (after kind, len, sub/flags, resv) */
    memcpy(peer_key, opt + 4, 8);
    return 0;
}

int mptcp_handle_capable(int conn_id, const uint8_t *opt, uint16_t optlen)
{
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS)
        return -EINVAL;
    if (!opt || optlen < MPTCP_CAPABLE_SYN_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP || (opt[2] >> 4) != MPTCP_CAPABLE)
        return -EINVAL;

    struct tcp_conn *c = &tcp_conns[conn_id];
    if (c->state == TCP_CLOSED)
        return -ECONNRESET;

    /* Extract the peer's key (sender's key in the option) */
    uint8_t peer_key[8];
    memcpy(peer_key, opt + 4, 8);

    /* Store the peer's key in the tcp_conn */
    memcpy(c->mptcp_rcv_key, peer_key, 8);
    c->mptcp_rcv_key_valid = 1;

    kprintf("[MPTCP] MP_CAPABLE received on conn %d: peer_key=%02x%02x%02x%02x...\n",
            conn_id, peer_key[0], peer_key[1], peer_key[2], peer_key[3]);

    /* If we already have our own key in the connection, the handshake
     * is complete (both sides have each other's keys). */
    if (c->mptcp_token != 0) {
        kprintf("[MPTCP] MP_CAPABLE handshake complete on conn %d "
                "(token=%u)\n", conn_id, c->mptcp_token);

        /* Also update the MPTCP connection if one exists */
        spinlock_acquire(&mptcp_lock);
        struct mptcp_conn *mc = mptcp_find_by_token(c->mptcp_token);
        if (mc) {
            memcpy(mc->rcv_key, peer_key, 8);
            mc->peer_token = 0; /* peer token would come from MP_JOIN */
            mc->established = 1;
        }
        spinlock_release(&mptcp_lock);
    }

    return 0;
}

/* ── Helper: compute truncated HMAC-SHA256 for MP_JOIN authentication ──
 * Per RFC 8684 §3.2.5, the message is:
 *   send_nonce(4) || recv_nonce(4) || send_addr_id(1) || recv_addr_id(1)
 * The key is the sender's 64-bit key.
 * Result: first 8 bytes of HMAC-SHA256.
 */
static void mptcp_join_compute_hmac(const uint8_t key[8],
                                     uint32_t send_nonce,
                                     uint32_t recv_nonce,
                                     uint8_t send_id,
                                     uint8_t recv_id,
                                     uint8_t hmac_out[8])
{
    uint8_t msg[10];
    uint8_t full_hmac[HMAC_SHA256_DIGEST_SIZE];

    /* Build message: send_nonce(4) || recv_nonce(4) || send_id(1) || recv_id(1) */
    msg[0] = (uint8_t)(send_nonce >> 24);
    msg[1] = (uint8_t)(send_nonce >> 16);
    msg[2] = (uint8_t)(send_nonce >> 8);
    msg[3] = (uint8_t)(send_nonce & 0xFF);
    msg[4] = (uint8_t)(recv_nonce >> 24);
    msg[5] = (uint8_t)(recv_nonce >> 16);
    msg[6] = (uint8_t)(recv_nonce >> 8);
    msg[7] = (uint8_t)(recv_nonce & 0xFF);
    msg[8] = send_id;
    msg[9] = recv_id;

    hmac_sha256(key, 8, msg, 10, full_hmac);
    memcpy(hmac_out, full_hmac, 8);
}

/* ── Token derivation from key (RFC 8684 §3.2.1) ──────────────────
 * Per RFC 8684, the token is the first 4 bytes of SHA-1(peer_key).
 * Since this kernel has no SHA-1 implementation, we use truncated
 * SHA-256 instead, which gives equivalent uniqueness properties.
 */
uint32_t mptcp_token_from_key(const uint8_t key[8])
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_hash(digest, key, 8);
    return ((uint32_t)digest[0] << 24) |
           ((uint32_t)digest[1] << 16) |
           ((uint32_t)digest[2] << 8)  |
           (uint32_t)digest[3];
}

/* ── Build MP_JOIN option for SYN ─────────────────────────────────
 * Format: kind=30, len=12, subtype=1, flags, addr_id, token(4), nonce(4)
 */
int mptcp_build_join_syn(uint8_t *buf, uint16_t *len,
                          uint8_t addr_id, uint32_t token, uint32_t nonce)
{
    if (!buf || !len)
        return -EINVAL;
    if (*len < MPTCP_JOIN_SYN_LEN)
        return -ENOSPC;

    buf[0] = TCPOPT_MPTCP;
    buf[1] = MPTCP_JOIN_SYN_LEN;
    buf[2] = (uint8_t)(MPTCP_JOIN << 4); /* subtype=1, flags=0 */
    buf[3] = addr_id;
    /* Token (network byte order) */
    buf[4] = (uint8_t)(token >> 24);
    buf[5] = (uint8_t)(token >> 16);
    buf[6] = (uint8_t)(token >> 8);
    buf[7] = (uint8_t)(token & 0xFF);
    /* Nonce (network byte order) */
    buf[8] = (uint8_t)(nonce >> 24);
    buf[9] = (uint8_t)(nonce >> 16);
    buf[10] = (uint8_t)(nonce >> 8);
    buf[11] = (uint8_t)(nonce & 0xFF);

    *len = MPTCP_JOIN_SYN_LEN;
    return 0;
}

/* ── Build MP_JOIN option for SYN+ACK ─────────────────────────────
 * Format: kind=30, len=16, subtype=1, flags, addr_id, nonce(4), hmac(8)
 */
int mptcp_build_join_synack(uint8_t *buf, uint16_t *len,
                             uint8_t addr_id, uint32_t nonce,
                             const uint8_t hmac[8])
{
    if (!buf || !len || !hmac)
        return -EINVAL;
    if (*len < MPTCP_JOIN_SYNACK_LEN)
        return -ENOSPC;

    buf[0] = TCPOPT_MPTCP;
    buf[1] = MPTCP_JOIN_SYNACK_LEN;
    buf[2] = (uint8_t)(MPTCP_JOIN << 4); /* subtype=1, flags=0 */
    buf[3] = addr_id;
    /* Nonce (network byte order) */
    buf[4] = (uint8_t)(nonce >> 24);
    buf[5] = (uint8_t)(nonce >> 16);
    buf[6] = (uint8_t)(nonce >> 8);
    buf[7] = (uint8_t)(nonce & 0xFF);
    /* Truncated HMAC */
    memcpy(buf + 8, hmac, 8);

    *len = MPTCP_JOIN_SYNACK_LEN;
    return 0;
}

/* ── Build MP_JOIN option for the 3rd ACK ─────────────────────────
 * Format: kind=30, len=12, subtype=1, flags, resv, hmac(8)
 */
int mptcp_build_join_ack(uint8_t *buf, uint16_t *len,
                          const uint8_t hmac[8])
{
    if (!buf || !len || !hmac)
        return -EINVAL;
    if (*len < MPTCP_JOIN_ACK_LEN)
        return -ENOSPC;

    buf[0] = TCPOPT_MPTCP;
    buf[1] = MPTCP_JOIN_ACK_LEN;
    buf[2] = (uint8_t)(MPTCP_JOIN << 4); /* subtype=1, flags=0 */
    buf[3] = 0; /* reserved */
    /* Truncated HMAC */
    memcpy(buf + 4, hmac, 8);

    *len = MPTCP_JOIN_ACK_LEN;
    return 0;
}

/* ── Parse received MP_JOIN SYN option ──────────────────────────── */
int mptcp_parse_join_syn(const uint8_t *opt, uint16_t optlen,
                          uint8_t *addr_id_out, uint32_t *token_out,
                          uint32_t *nonce_out)
{
    if (!opt || !addr_id_out || !token_out || !nonce_out)
        return -EINVAL;
    if (optlen < MPTCP_JOIN_SYN_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP)
        return -EINVAL;
    if ((opt[2] >> 4) != MPTCP_JOIN)
        return -EINVAL;

    *addr_id_out = opt[3];
    *token_out = ((uint32_t)opt[4] << 24) |
                 ((uint32_t)opt[5] << 16) |
                 ((uint32_t)opt[6] << 8)  |
                 (uint32_t)opt[7];
    *nonce_out = ((uint32_t)opt[8] << 24) |
                 ((uint32_t)opt[9] << 16) |
                 ((uint32_t)opt[10] << 8) |
                 (uint32_t)opt[11];
    return 0;
}

/* ── Parse received MP_JOIN SYN+ACK option ──────────────────────── */
int mptcp_parse_join_synack(const uint8_t *opt, uint16_t optlen,
                             uint8_t *addr_id_out, uint32_t *nonce_out,
                             uint8_t hmac_out[8])
{
    if (!opt || !addr_id_out || !nonce_out || !hmac_out)
        return -EINVAL;
    if (optlen < MPTCP_JOIN_SYNACK_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP)
        return -EINVAL;
    if ((opt[2] >> 4) != MPTCP_JOIN)
        return -EINVAL;

    *addr_id_out = opt[3];
    *nonce_out = ((uint32_t)opt[4] << 24) |
                 ((uint32_t)opt[5] << 16) |
                 ((uint32_t)opt[6] << 8)  |
                 (uint32_t)opt[7];
    memcpy(hmac_out, opt + 8, 8);
    return 0;
}

/* ── Parse received MP_JOIN ACK option ──────────────────────────── */
int mptcp_parse_join_ack(const uint8_t *opt, uint16_t optlen,
                          uint8_t hmac_out[8])
{
    if (!opt || !hmac_out)
        return -EINVAL;
    if (optlen < MPTCP_JOIN_ACK_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP)
        return -EINVAL;
    if ((opt[2] >> 4) != MPTCP_JOIN)
        return -EINVAL;

    memcpy(hmac_out, opt + 4, 8);
    return 0;
}

/* ── Handle received MP_JOIN option on a SYN (server side) ──────
 * Called when an MP_JOIN option is received on a SYN packet.
 * Looks up the MPTCP connection by token, stores the peer's nonce
 * and addr_id, and records enough state so the TCP stack can
 * include the correct MP_JOIN SYN+ACK response.
 * Returns 0 on success, negative errno on error. */
int mptcp_handle_join(int conn_id, const uint8_t *opt, uint16_t optlen)
{
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS)
        return -EINVAL;
    if (!opt || optlen < MPTCP_JOIN_SYN_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP)
        return -EINVAL;
    if ((opt[2] >> 4) != MPTCP_JOIN)
        return -EINVAL;

    /* Parse the MP_JOIN SYN option */
    uint8_t  cli_addr_id;
    uint32_t join_token;
    uint32_t cli_nonce;
    int ret = mptcp_parse_join_syn(opt, optlen,
                                    &cli_addr_id, &join_token, &cli_nonce);
    if (ret < 0)
        return ret;

    /* Look up the MPTCP connection by token */
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = NULL;
    for (int i = 0; i < MPTCP_MAX_CONNS; i++) {
        if (mptcp_conns[i].used) {
            uint32_t expected_token = mptcp_token_from_key(mptcp_conns[i].snd_key);
            if (expected_token == join_token) {
                mc = &mptcp_conns[i];
                break;
            }
        }
    }
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[MPTCP] MP_JOIN SYN: token %u not found (connection lookup failed)\n",
                join_token);
        return -ENOENT;
    }

    /* Check we haven't exceeded the maximum number of subflows */
    if (mc->num_subflows >= MPTCP_MAX_SUBFLOWS) {
        spinlock_release(&mptcp_lock);
        kprintf("[MPTCP] MP_JOIN SYN: token %u max subflows reached\n", join_token);
        return -ENOSPC;
    }

    /* Find a free subflow slot */
    int slot = -1;
    for (int i = 0; i < MPTCP_MAX_SUBFLOWS; i++) {
        if (!mc->subflows[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_release(&mptcp_lock);
        return -ENOMEM;
    }

    /* Generate our (server's) nonce using timer ticks */
    uint32_t srv_nonce = (uint32_t)(timer_get_ticks() ^
                         ((uint64_t)timer_get_ticks() >> 20));
    srv_nonce ^= (uint32_t)((uint64_t)join_token >> 8);
    srv_nonce ^= mptcp_next_token++;

    /* Use the connection's number of subflows as the server's addr_id
     * (simple scheme: each new subflow gets the next addr_id) */
    uint8_t srv_addr_id = (uint8_t)(slot + 1);

    /* Compute the HMAC for the SYN+ACK response.
     * Server's perspective: key = our snd_key, send_nonce = srv_nonce,
     * recv_nonce = cli_nonce, send_id = srv_addr_id, recv_id = cli_addr_id */
    uint8_t join_hmac[8];
    mptcp_join_compute_hmac(mc->snd_key,
                            srv_nonce, cli_nonce,
                            srv_addr_id, cli_addr_id,
                            join_hmac);

    /* Store the pending join state in the subflow slot */
    struct mptcp_subflow *sf = &mc->subflows[slot];
    memset(sf, 0, sizeof(*sf));
    sf->used = 1;
    sf->token = join_token;
    sf->conn_id = conn_id;
    sf->join_nonce = cli_nonce;       /* Peer's (client) nonce */
    sf->join_local_nonce = srv_nonce; /* Our nonce */
    sf->join_id = cli_addr_id;        /* Peer's addr_id */
    sf->join_local_id = srv_addr_id;  /* Our addr_id */
    memcpy(sf->join_hmac, join_hmac, 8);

    /* Bump num_subflows to include this pending subflow */
    mc->num_subflows++;

    kprintf("[MPTCP] MP_JOIN SYN accepted: conn=%d join_token=%u "
            "cli_nonce=%u srv_nonce=%u cli_addr_id=%u srv_addr_id=%u\n",
            conn_id, join_token, cli_nonce, srv_nonce,
            (unsigned)cli_addr_id, (unsigned)srv_addr_id);

    spinlock_release(&mptcp_lock);

    /* Also update the TCP connection's MPTCP metadata */
    struct tcp_conn *c = &tcp_conns[conn_id];
    c->mptcp_token = 0; /* This is a subflow, not primary */
    memcpy(c->mptcp_rcv_key, mc->rcv_key, 8);
    c->mptcp_rcv_key_valid = 1;

    return 0;
}

/* ── DSS build: Build a DSS option for a data segment ─────────────
 * Per RFC 8684 §3.3, the DSS option carries:
 *   - Optional Data ACK (4 or 8 bytes)
 *   - Optional Data Sequence Number (4 or 8 bytes)
 *   - Optional Subflow Sequence Number (2 or 4 bytes)
 *   - Optional Data-Level Length (2 bytes) — present when DSN is present,
 *     or standalone (but RFC says it's always with DSN or SSN)
 *   - Optional Checksum (2 bytes)
 *
 * We always use 8-byte Data ACK, 8-byte DSN, 4-byte SSN for
 * maximum compatibility (Linux MPTCP reference implementation
 * also uses the full-width variants).
 *
 * Returns 0 on success, negative errno on failure.
 * On success, *len is updated to the bytes written. */
int mptcp_build_dss(uint8_t *buf, uint16_t *len,
                     uint64_t data_ack, int data_ack_valid,
                     uint64_t data_seq, int data_seq_valid,
                     uint32_t subflow_seq, int subflow_seq_valid,
                     uint16_t data_len, int include_checksum)
{
    uint8_t flags_lo = 0;
    uint16_t opt_len = MPTCP_DSS_MIN_LEN;
    int offset = MPTCP_DSS_MIN_LEN;  /* past the 4-byte header */

    if (!buf || !len)
        return -EINVAL;

    /* Determine flags and option length */
    if (data_ack_valid) {
        flags_lo |= MPTCP_DSS_FLAG_A | MPTCP_DSS_FLAG_A8; /* 8-byte data ACK */
        opt_len += 8;  /* 8 bytes for the data ACK */
    }
    if (data_seq_valid) {
        flags_lo |= MPTCP_DSS_FLAG_M; /* 8-byte DSN */
        opt_len += 8;  /* DSN = 8 bytes */
    }
    if (subflow_seq_valid) {
        flags_lo |= MPTCP_DSS_FLAG_F; /* 4-byte SSN */
        opt_len += 4;  /* SSN = 4 bytes */
    }
    if (data_seq_valid || subflow_seq_valid) {
        /* Data-Level Length is 2 bytes, present when either DSN or SSN is present */
        opt_len += 2;
    }
    if (include_checksum) {
        flags_lo |= MPTCP_DSS_FLAG_C;
        opt_len += MPTCP_DSS_CKSUM_LEN;
    }

    if (*len < opt_len)
        return -ENOSPC;

    /* Build the option */
    buf[0] = TCPOPT_MPTCP;                              /* kind = 30 */
    buf[1] = (uint8_t)opt_len;                          /* length */
    buf[2] = (uint8_t)(MPTCP_DSS << 4);                 /* subtype = 2 */
    buf[3] = flags_lo;                                  /* data flags */

    /* Data ACK (8 bytes, network byte order) */
    if (data_ack_valid && (flags_lo & MPTCP_DSS_FLAG_A)) {
        buf[offset + 0] = (uint8_t)(data_ack >> 56);
        buf[offset + 1] = (uint8_t)(data_ack >> 48);
        buf[offset + 2] = (uint8_t)(data_ack >> 40);
        buf[offset + 3] = (uint8_t)(data_ack >> 32);
        buf[offset + 4] = (uint8_t)(data_ack >> 24);
        buf[offset + 5] = (uint8_t)(data_ack >> 16);
        buf[offset + 6] = (uint8_t)(data_ack >> 8);
        buf[offset + 7] = (uint8_t)(data_ack & 0xFF);
        offset += 8;
    }

    /* Data Sequence Number (8 bytes, network byte order) */
    if (data_seq_valid && (flags_lo & MPTCP_DSS_FLAG_M)) {
        buf[offset + 0] = (uint8_t)(data_seq >> 56);
        buf[offset + 1] = (uint8_t)(data_seq >> 48);
        buf[offset + 2] = (uint8_t)(data_seq >> 40);
        buf[offset + 3] = (uint8_t)(data_seq >> 32);
        buf[offset + 4] = (uint8_t)(data_seq >> 24);
        buf[offset + 5] = (uint8_t)(data_seq >> 16);
        buf[offset + 6] = (uint8_t)(data_seq >> 8);
        buf[offset + 7] = (uint8_t)(data_seq & 0xFF);
        offset += 8;
    }

    /* Subflow Sequence Number (4 bytes, network byte order) */
    if (subflow_seq_valid && (flags_lo & MPTCP_DSS_FLAG_F)) {
        buf[offset + 0] = (uint8_t)(subflow_seq >> 24);
        buf[offset + 1] = (uint8_t)(subflow_seq >> 16);
        buf[offset + 2] = (uint8_t)(subflow_seq >> 8);
        buf[offset + 3] = (uint8_t)(subflow_seq & 0xFF);
        offset += 4;
    }

    /* Data-Level Length (2 bytes, network byte order) — present when DSN or SSN */
    if (data_seq_valid || subflow_seq_valid) {
        buf[offset + 0] = (uint8_t)(data_len >> 8);
        buf[offset + 1] = (uint8_t)(data_len & 0xFF);
        offset += 2;
    }

    /* Checksum (2 bytes, network byte order) — zero placeholder */
    if (include_checksum) {
        buf[offset + 0] = 0;
        buf[offset + 1] = 0;
        offset += 2;
    }

    (void)offset; /* used for systematic tracking */

    *len = opt_len;
    return 0;
}

/* ── DSS parse: Parse a received DSS option ──────────────────────
 * Parses the DSS option and extracts all present fields.
 * Caller should zero the valid flags before calling; on success
 * each valid flag is set to 1 if the corresponding field was
 * present in the option.
 * Returns 0 on success, negative errno on failure. */
int mptcp_parse_dss(const uint8_t *opt, uint16_t optlen,
                     uint64_t *data_ack_out, int *data_ack_valid,
                     uint64_t *data_seq_out, int *data_seq_valid,
                     uint32_t *subflow_seq_out, int *subflow_seq_valid,
                     uint16_t *data_len_out, int *include_checksum)
{
    uint8_t flags_lo;
    int offset = MPTCP_DSS_MIN_LEN;

    if (!opt || !data_ack_valid || !data_seq_valid || !subflow_seq_valid)
        return -EINVAL;
    if (optlen < MPTCP_DSS_MIN_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP)
        return -EINVAL;
    if ((opt[2] >> 4) != MPTCP_DSS)
        return -EINVAL;

    /* Reset valid flags */
    *data_ack_valid = 0;
    *data_seq_valid = 0;
    *subflow_seq_valid = 0;
    if (include_checksum)
        *include_checksum = 0;

    flags_lo = opt[3];

    /* Check if checksum is present (must account for it in option length) */
    int ck = (flags_lo & MPTCP_DSS_FLAG_C) ? 1 : 0;
    if (include_checksum)
        *include_checksum = ck;

    /* Data ACK (4 or 8 bytes) */
    if (flags_lo & MPTCP_DSS_FLAG_A) {
        int ack_bytes = (flags_lo & MPTCP_DSS_FLAG_A8) ? 8 : 4;
        if ((int)optlen < offset + ack_bytes)
            return -EINVAL;
        if (data_ack_out) {
            if (ack_bytes == 8) {
                *data_ack_out = ((uint64_t)opt[offset + 0] << 56) |
                                ((uint64_t)opt[offset + 1] << 48) |
                                ((uint64_t)opt[offset + 2] << 40) |
                                ((uint64_t)opt[offset + 3] << 32) |
                                ((uint64_t)opt[offset + 4] << 24) |
                                ((uint64_t)opt[offset + 5] << 16) |
                                ((uint64_t)opt[offset + 6] << 8)  |
                                (uint64_t)opt[offset + 7];
            } else {
                *data_ack_out = ((uint64_t)opt[offset + 0] << 24) |
                                ((uint64_t)opt[offset + 1] << 16) |
                                ((uint64_t)opt[offset + 2] << 8)  |
                                (uint64_t)opt[offset + 3];
            }
        }
        *data_ack_valid = 1;
        offset += ack_bytes;
    }

    /* Data Sequence Number (4 or 8 bytes) */
    if (flags_lo & MPTCP_DSS_FLAG_M) {
        int dsn_bytes = (flags_lo & MPTCP_DSS_FLAG_M4) ? 4 : 8;
        if ((int)optlen < offset + dsn_bytes)
            return -EINVAL;
        if (data_seq_out) {
            if (dsn_bytes == 8) {
                *data_seq_out = ((uint64_t)opt[offset + 0] << 56) |
                                ((uint64_t)opt[offset + 1] << 48) |
                                ((uint64_t)opt[offset + 2] << 40) |
                                ((uint64_t)opt[offset + 3] << 32) |
                                ((uint64_t)opt[offset + 4] << 24) |
                                ((uint64_t)opt[offset + 5] << 16) |
                                ((uint64_t)opt[offset + 6] << 8)  |
                                (uint64_t)opt[offset + 7];
            } else {
                *data_seq_out = ((uint64_t)opt[offset + 0] << 24) |
                                ((uint64_t)opt[offset + 1] << 16) |
                                ((uint64_t)opt[offset + 2] << 8)  |
                                (uint64_t)opt[offset + 3];
            }
        }
        *data_seq_valid = 1;
        offset += dsn_bytes;
    }

    /* Subflow Sequence Number (2 or 4 bytes) */
    if (flags_lo & MPTCP_DSS_FLAG_F) {
        int ssn_bytes = (flags_lo & MPTCP_DSS_FLAG_F2) ? 2 : 4;
        if ((int)optlen < offset + ssn_bytes)
            return -EINVAL;
        if (subflow_seq_out) {
            if (ssn_bytes == 4) {
                *subflow_seq_out = ((uint32_t)opt[offset + 0] << 24) |
                                   ((uint32_t)opt[offset + 1] << 16) |
                                   ((uint32_t)opt[offset + 2] << 8)  |
                                   (uint32_t)opt[offset + 3];
            } else {
                *subflow_seq_out = ((uint32_t)opt[offset + 0] << 8) |
                                   (uint32_t)opt[offset + 1];
            }
        }
        *subflow_seq_valid = 1;
        offset += ssn_bytes;
    }

    /* Data-Level Length (2 bytes, present when DSN or SSN is present) */
    if ((flags_lo & (MPTCP_DSS_FLAG_M | MPTCP_DSS_FLAG_F)) &&
        data_len_out) {
        if ((int)optlen < offset + 2)
            return -EINVAL;
        *data_len_out = ((uint16_t)opt[offset] << 8) |
                        (uint16_t)opt[offset + 1];
        offset += 2;
    }

    /* Skip checksum if present */
    if (ck) {
        if ((int)optlen < offset + MPTCP_DSS_CKSUM_LEN)
            return -EINVAL;
        offset += MPTCP_DSS_CKSUM_LEN;
    }

    (void)optlen;
    (void)offset;

    return 0;
}

/* ── Handle received DSS option ──────────────────────────────────
 * Called from the TCP stack when a DSS option is received on an
 * MPTCP subflow.  Performs two roles:
 *
 * 1. Data ACK processing: If the DSS carries a Data ACK, this
 *    tells us how much MPTCP data the peer has received.  We
 *    update snd_data_ack on the MPTCP connection.
 *
 * 2. Data sequence mapping: If the DSS carries a Data Sequence
 *    Number + Subflow Sequence Number + Data Length, it maps
 *    the subflow's bytes to the MPTCP data stream.  We record
 *    this mapping on the subflow so the receiver can reassemble
 *    data from multiple subflows in the correct order.
 *
 * Returns 0 on success, negative errno on failure. */
int mptcp_handle_dss(int conn_id, const uint8_t *opt, uint16_t optlen,
                      uint32_t seq, uint32_t ack)
{
    int ret;
    int data_ack_valid = 0, data_seq_valid = 0, subflow_seq_valid = 0;
    int include_checksum = 0;
    uint64_t data_ack = 0, data_seq = 0;
    uint32_t subflow_seq = 0;
    uint16_t data_len = 0;

    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS)
        return -EINVAL;
    if (!opt || optlen < MPTCP_DSS_MIN_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP)
        return -EINVAL;
    if ((opt[2] >> 4) != MPTCP_DSS)
        return -EINVAL;

    ret = mptcp_parse_dss(opt, optlen,
                           &data_ack, &data_ack_valid,
                           &data_seq, &data_seq_valid,
                           &subflow_seq, &subflow_seq_valid,
                           &data_len, &include_checksum);
    if (ret < 0)
        return ret;

    /* Look up the MPTCP connection via the TCP connection's token */
    struct tcp_conn *c = &tcp_conns[conn_id];
    uint32_t token = c->mptcp_token;

    if (token == 0) {
        /* Not an MPTCP subflow — ignore DSS */
        return 0;
    }

    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        return 0;  /* Not fatal — connection may have been closed */
    }

    /* Role 1: Process Data ACK — peer has acknowledged data-level
     * bytes up to data_ack.  Update our send-side tracking. */
    if (data_ack_valid) {
        if (data_ack > mc->snd_data_ack) {
            mc->snd_data_ack = data_ack;
            kprintf("[MPTCP-DSS] Data ACK updated: token=%u data_ack=%lu\n",
                    token, (unsigned long)data_ack);
        }
    }

    /* Role 2: Process Data Sequence Mapping — this packet's data
     * maps to data_seq (data-level) starting at subflow_seq on the
     * subflow.  Find the subflow and record the mapping. */
    if (data_seq_valid && subflow_seq_valid && data_len > 0) {
        /* Find the subflow matching this TCP connection */
        for (uint8_t i = 0; i < mc->num_subflows; i++) {
            struct mptcp_subflow *sf = &mc->subflows[i];
            if (!sf->used)
                continue;
            if (sf->conn_id == conn_id) {
                /* Record the DSS mapping */
                sf->dss_data_seq = data_seq;
                sf->dss_subflow_seq = subflow_seq;
                sf->dss_mapped_len = data_len;

                /* Update receive data sequence tracking */
                uint64_t expected_seq = mc->rcv_data_seq;
                if (data_seq == expected_seq) {
                    /* In-order data — advance rcv_data_seq */
                    mc->rcv_data_seq = data_seq + data_len;
                    mc->rcv_data_ack = mc->rcv_data_seq;
                }

                kprintf("[MPTCP-DSS] Mapping: token=%u conn_id=%d "
                        "data_seq=%lu subflow_seq=%u len=%u "
                        "(rcv_data_seq=%lu)\n",
                        token, conn_id,
                        (unsigned long)data_seq, subflow_seq,
                        (unsigned)data_len,
                        (unsigned long)mc->rcv_data_seq);
                break;
            }
        }
    }

    spinlock_release(&mptcp_lock);
    return 0;
}

EXPORT_SYMBOL(mptcp_init);
EXPORT_SYMBOL(mptcp_create);
EXPORT_SYMBOL(mptcp_add_subflow);
EXPORT_SYMBOL(mptcp_remove_subflow);
EXPORT_SYMBOL(mptcp_send);
EXPORT_SYMBOL(mptcp_recv);
EXPORT_SYMBOL(mptcp_close);
/* ── Implement: mptcp_subflow_create ────────────────── */
int mptcp_subflow_create(uint32_t token, uint32_t addr, uint16_t port)
{
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_subflow_create: not initialized\n");
        return -ENOSYS;
    }
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_subflow_create: token %u not found\n", token);
        return -EINVAL;
    }
    if (mc->num_subflows >= MPTCP_MAX_SUBFLOWS) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_subflow_create: max subflows reached for token %u\n", token);
        return -ENOSPC;
    }
    int slot = mptcp_find_free();
    if (slot < 0) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_subflow_create: no free subflow slot\n");
        return -ENOMEM;
    }
    struct mptcp_subflow *sf = &mc->subflows[mc->num_subflows++];
    sf->used = 1;
    sf->token = token;
    sf->conn_id = slot;
    memcpy(sf->key, mc->snd_key, 8);
    /* Initialize DSS tracking fields */
    sf->dss_data_seq = 0;
    sf->dss_subflow_seq = 0;
    sf->dss_mapped_len = 0;
    kprintf("[mptcp] mptcp_subflow_create: token=%u addr=%u:%u (stub)\n",
            token, addr, (unsigned)port);
    spinlock_release(&mptcp_lock);
    return 0;
}

/* ── Implement: mptcp_add_addr ──────────────────
 * Track an advertised address in the MPTCP connection's address table.
 * This stores the address locally so it can be announced to the peer
 * via the ADD_ADDR TCP option, and also records addresses received
 * from the peer for potential subflow creation.
 * Returns 0 on success, negative errno on failure.
 * If the address already exists, returns 0 (idempotent). */
int mptcp_add_addr(uint32_t token, uint32_t addr, uint16_t port, uint8_t flags)
{
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_add_addr: not initialized\n");
        return -ENOSYS;
    }
    if (addr == 0) {
        kprintf("[mptcp] mptcp_add_addr: invalid address 0\n");
        return -EINVAL;
    }

    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_add_addr: token %u not found\n", token);
        return -EINVAL;
    }

    /* Check if this address is already tracked */
    for (uint8_t i = 0; i < mc->num_addrs; i++) {
        if (mc->addrs[i].used && mc->addrs[i].addr == addr) {
            /* Already exists — update flags and port */
            mc->addrs[i].flags = flags;
            mc->addrs[i].port = port;
            kprintf("[mptcp] mptcp_add_addr: token=%u addr=%u:%u already tracked, "
                    "updated flags=0x%02x\n",
                    token, addr, (unsigned)port, (unsigned)flags);
            spinlock_release(&mptcp_lock);
            return 0;
        }
    }

    /* Find a free slot */
    int slot = -1;
    for (uint8_t i = 0; i < MPTCP_MAX_ADDRS; i++) {
        if (!mc->addrs[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_add_addr: token=%u addr table full\n", token);
        return -ENOSPC;
    }

    /* Assign address ID — use slot index as addr_id for simplicity */
    mc->addrs[slot].used = 1;
    mc->addrs[slot].addr_id = (uint8_t)slot;
    mc->addrs[slot].addr = addr;
    mc->addrs[slot].port = port;
    mc->addrs[slot].flags = flags | MPTCP_ADDR_FLAG_IPV4;

    if (slot >= mc->num_addrs)
        mc->num_addrs = (uint8_t)(slot + 1);

    kprintf("[mptcp] mptcp_add_addr: token=%u addr=%u:%u flags=0x%02x "
            "addr_id=%u (stored)\n",
            token, addr, (unsigned)port, (unsigned)flags,
            (unsigned)mc->addrs[slot].addr_id);

    spinlock_release(&mptcp_lock);
    return 0;
}

/* ── Implement: mptcp_remove_addr ──────────────────
 * Remove an advertised address from the MPTCP connection's address
 * table by its address ID.  This is called when the peer sends
 * a REMOVE_ADDR option, or locally to stop advertising an address.
 * Returns 0 on success, negative errno on failure. */
int mptcp_remove_addr(uint32_t token, uint32_t addr_id)
{
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_remove_addr: not initialized\n");
        return -ENOSYS;
    }

    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_remove_addr: token %u not found\n", token);
        return -EINVAL;
    }

    if (addr_id >= MPTCP_MAX_ADDRS || !mc->addrs[(int)addr_id].used) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_remove_addr: token=%u addr_id=%u not found\n",
                token, (unsigned)addr_id);
        return -ENOENT;
    }

    mc->addrs[(int)addr_id].used = 0;

    /* Compact num_addrs if the last slot was freed */
    while (mc->num_addrs > 0 && !mc->addrs[mc->num_addrs - 1].used)
        mc->num_addrs--;

    kprintf("[mptcp] mptcp_remove_addr: token=%u addr_id=%u (removed)\n",
            token, (unsigned)addr_id);

    spinlock_release(&mptcp_lock);
    return 0;
}

/* ── mptcp_has_addr: Check if an address is already advertised ── */
int mptcp_has_addr(uint32_t token, uint32_t addr)
{
    if (!mptcp_initialized)
        return 0;

    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        return 0;
    }

    for (uint8_t i = 0; i < MPTCP_MAX_ADDRS; i++) {
        if (mc->addrs[i].used && mc->addrs[i].addr == addr) {
            spinlock_release(&mptcp_lock);
            return 1;
        }
    }
    spinlock_release(&mptcp_lock);
    return 0;
}

/* ── mptcp_find_free_addr: Find a free address ID slot ────────── */
int mptcp_find_free_addr(uint32_t token, uint8_t *addr_id_out)
{
    if (!mptcp_initialized || !addr_id_out)
        return -EINVAL;

    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        return -EINVAL;
    }

    for (uint8_t i = 0; i < MPTCP_MAX_ADDRS; i++) {
        if (!mc->addrs[i].used) {
            *addr_id_out = i;
            spinlock_release(&mptcp_lock);
            return 0;
        }
    }

    spinlock_release(&mptcp_lock);
    return -ENOSPC;
}

/* ── Build ADD_ADDR option buffer (IPv4) ─────────────────────────
 * Builds the TCP option bytes for ADD_ADDR (subtype 3) with an IPv4
 * address and optional port.
 *
 * Option format:
 *   byte[0]   = TCPOPT_MPTCP (30)
 *   byte[1]   = length (10 without port, 12 with port)
 *   byte[2]   = (MPTCP_ADD_ADDR << 4) | (flags & 0x0F)
 *   byte[3]   = addr_id
 *   byte[4]   = IP version (4 for IPv4)
 *   byte[5]   = reserved (0)
 *   bytes[6-9] = IPv4 address (network byte order)
 *   bytes[10-11] = port (optional, network byte order, only if port != 0)
 *
 * Returns bytes written to buf, or negative errno on error. */
int mptcp_build_add_addr_v4(uint8_t *buf, uint16_t *len,
                             uint8_t addr_id, uint32_t addr,
                             uint16_t port, uint8_t flags)
{
    uint16_t opt_len;

    if (!buf || !len)
        return -EINVAL;

    if (port != 0)
        opt_len = MPTCP_ADD_ADDR4_LEN_PORT;
    else
        opt_len = MPTCP_ADD_ADDR4_LEN;

    if (*len < opt_len)
        return -ENOSPC;

    buf[0] = TCPOPT_MPTCP;
    buf[1] = (uint8_t)opt_len;
    buf[2] = (uint8_t)((MPTCP_ADD_ADDR << 4) | (flags & 0x0F));
    buf[3] = addr_id;
    buf[4] = 4;          /* IP version = IPv4 */
    buf[5] = 0;          /* reserved */

    /* IPv4 address in network byte order */
    buf[6] = (uint8_t)(addr >> 24);
    buf[7] = (uint8_t)(addr >> 16);
    buf[8] = (uint8_t)(addr >> 8);
    buf[9] = (uint8_t)(addr & 0xFF);

    if (port != 0) {
        buf[10] = (uint8_t)(port >> 8);
        buf[11] = (uint8_t)(port & 0xFF);
    }

    *len = opt_len;
    return (int)opt_len;
}

/* ── Build REMOVE_ADDR option buffer ─────────────────────────────
 * Builds the TCP option bytes for REMOVE_ADDR (subtype 4) with a
 * single address ID.
 *
 * Option format:
 *   byte[0] = TCPOPT_MPTCP (30)
 *   byte[1] = 4 (length)
 *   byte[2] = (MPTCP_REMOVE_ADDR << 4) | 0x00
 *   byte[3] = addr_id
 *
 * Returns bytes written to buf, or negative errno on error. */
int mptcp_build_remove_addr(uint8_t *buf, uint16_t *len, uint8_t addr_id)
{
    if (!buf || !len)
        return -EINVAL;
    if (*len < MPTCP_REMOVE_ADDR_MIN_LEN)
        return -ENOSPC;

    buf[0] = TCPOPT_MPTCP;
    buf[1] = MPTCP_REMOVE_ADDR_MIN_LEN;
    buf[2] = (uint8_t)(MPTCP_REMOVE_ADDR << 4);
    buf[3] = addr_id;

    *len = MPTCP_REMOVE_ADDR_MIN_LEN;
    return MPTCP_REMOVE_ADDR_MIN_LEN;
}

/* ── Parse received ADD_ADDR option ──────────────────────────────
 * Parses an ADD_ADDR TCP option and extracts the address information.
 * Currently supports IPv4 only.
 *
 * Returns 0 on success, negative errno on error. */
int mptcp_parse_add_addr(const uint8_t *opt, uint16_t optlen,
                          uint8_t *addr_id_out, uint32_t *addr_out,
                          uint16_t *port_out, uint8_t *flags_out)
{
    if (!opt || !addr_id_out || !addr_out || !port_out || !flags_out)
        return -EINVAL;
    if (optlen < MPTCP_ADD_ADDR4_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP)
        return -EINVAL;
    if ((opt[2] >> 4) != MPTCP_ADD_ADDR)
        return -EINVAL;

    /* Byte 2 lower nibble: flags */
    *flags_out = opt[2] & 0x0F;

    /* Byte 3: addr_id */
    *addr_id_out = opt[3];

    /* Byte 4: IP version — only support IPv4 for now */
    if (opt[4] != 4)
        return -EAFNOSUPPORT;

    /* Check if port is present (length check) */
    *addr_out = ((uint32_t)opt[6] << 24) |
                ((uint32_t)opt[7] << 16) |
                ((uint32_t)opt[8] << 8) |
                (uint32_t)opt[9];

    if (optlen >= MPTCP_ADD_ADDR4_LEN_PORT && opt[1] >= MPTCP_ADD_ADDR4_LEN_PORT)
        *port_out = ((uint16_t)opt[10] << 8) | (uint16_t)opt[11];
    else
        *port_out = 0;

    return 0;
}

/* ── Parse received REMOVE_ADDR option ───────────────────────────
 * Parses a REMOVE_ADDR TCP option and extracts the address ID.
 *
 * Returns 0 on success, negative errno on error. */
int mptcp_parse_remove_addr(const uint8_t *opt, uint16_t optlen,
                             uint8_t *addr_id_out)
{
    if (!opt || !addr_id_out)
        return -EINVAL;
    if (optlen < MPTCP_REMOVE_ADDR_MIN_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP)
        return -EINVAL;
    if ((opt[2] >> 4) != MPTCP_REMOVE_ADDR)
        return -EINVAL;

    *addr_id_out = opt[3];
    return 0;
}

/* ── Handle received ADD_ADDR on a connection ────────────────────
 * Called from the TCP stack when an ADD_ADDR option is received on
 * an MPTCP subflow.  Stores the advertised address in the MPTCP
 * connection's address table for potential subflow creation.
 *
 * Returns 0 on success, negative errno on error. */
int mptcp_handle_add_addr(int conn_id, const uint8_t *opt, uint16_t optlen)
{
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS)
        return -EINVAL;
    if (!opt || optlen < MPTCP_ADD_ADDR4_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP || (opt[2] >> 4) != MPTCP_ADD_ADDR)
        return -EINVAL;

    struct tcp_conn *c = &tcp_conns[conn_id];
    if (c->state == TCP_CLOSED)
        return -ECONNRESET;

    uint32_t token = c->mptcp_token;
    if (token == 0) {
        kprintf("[mptcp] mptcp_handle_add_addr: conn %d not an MPTCP subflow\n",
                conn_id);
        return -EINVAL;
    }

    uint8_t  addr_id;
    uint32_t addr;
    uint16_t port;
    uint8_t  flags;
    int ret;

    ret = mptcp_parse_add_addr(opt, optlen, &addr_id, &addr, &port, &flags);
    if (ret < 0) {
        kprintf("[mptcp] mptcp_handle_add_addr: parse error %d on conn %d\n",
                ret, conn_id);
        return ret;
    }

    /* Set the echo flag to indicate this is a received ADD_ADDR */
    flags |= MPTCP_ADDR_FLAG_ECHO;

    /* Store the advertised address */
    ret = mptcp_add_addr(token, addr, port, flags);
    if (ret < 0) {
        kprintf("[mptcp] mptcp_handle_add_addr: failed to store addr "
                "for token %u: %d\n", token, ret);
        return ret;
    }

    kprintf("[mptcp] ADD_ADDR received on conn %d: addr_id=%u "
            "addr=%u.%u.%u.%u port=%u flags=0x%02x\n",
            conn_id,
            (unsigned)addr_id,
            (unsigned)(addr >> 24), (unsigned)(addr >> 16 & 0xFF),
            (unsigned)(addr >> 8 & 0xFF), (unsigned)(addr & 0xFF),
            (unsigned)port, (unsigned)flags);

    return 0;
}

/* ── Handle received REMOVE_ADDR on a connection ─────────────────
 * Called from the TCP stack when a REMOVE_ADDR option is received on
 * an MPTCP subflow.  Removes the address from the MPTCP connection's
 * address table.
 *
 * Returns 0 on success, negative errno on error. */
int mptcp_handle_remove_addr(int conn_id, const uint8_t *opt, uint16_t optlen)
{
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS)
        return -EINVAL;
    if (!opt || optlen < MPTCP_REMOVE_ADDR_MIN_LEN)
        return -EINVAL;
    if (opt[0] != TCPOPT_MPTCP || (opt[2] >> 4) != MPTCP_REMOVE_ADDR)
        return -EINVAL;

    struct tcp_conn *c = &tcp_conns[conn_id];
    if (c->state == TCP_CLOSED)
        return -ECONNRESET;

    uint32_t token = c->mptcp_token;
    if (token == 0) {
        kprintf("[mptcp] mptcp_handle_remove_addr: conn %d not MPTCP\n",
                conn_id);
        return -EINVAL;
    }

    uint8_t addr_id;
    int ret = mptcp_parse_remove_addr(opt, optlen, &addr_id);
    if (ret < 0) {
        kprintf("[mptcp] mptcp_handle_remove_addr: parse error %d on conn %d\n",
                ret, conn_id);
        return ret;
    }

    ret = mptcp_remove_addr(token, (uint32_t)addr_id);
    if (ret < 0) {
        kprintf("[mptcp] mptcp_handle_remove_addr: token=%u addr_id=%u "
                "remove error %d\n", token, (unsigned)addr_id, ret);
        return ret;
    }

    kprintf("[mptcp] REMOVE_ADDR received on conn %d: addr_id=%u\n",
            conn_id, (unsigned)addr_id);

    return 0;
}

/* ── Implement: mptcp_priority ────────────────── */
int mptcp_priority(uint32_t token, uint32_t addr_id, uint8_t backup)
{
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_priority: not initialized\n");
        return -ENOSYS;
    }
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_priority: token %u not found\n", token);
        return -EINVAL;
    }
    kprintf("[mptcp] mptcp_priority: token=%u addr_id=%u backup=%u (stub)\n",
            token, addr_id, (unsigned)backup);
    spinlock_release(&mptcp_lock);
    return -EOPNOTSUPP;
}

/* ── Implement: mptcp_fastclose ────────────────── */
int mptcp_fastclose(uint32_t token)
{
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_fastclose: not initialized\n");
        return -ENOSYS;
    }
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_fastclose: token %u not found\n", token);
        return -EINVAL;
    }
    kprintf("[mptcp] mptcp_fastclose: token=%u (stub)\n", token);
    /* Close the connection via mptcp_close */
    memset(mc, 0, sizeof(*mc));
    spinlock_release(&mptcp_lock);
    return 0;
}

/* ── Implement: mptcp_reset ────────────────── */
int mptcp_reset(uint32_t token, uint32_t addr_id)
{
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_reset: not initialized\n");
        return -ENOSYS;
    }
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_reset: token %u not found\n", token);
        return -EINVAL;
    }
    kprintf("[mptcp] mptcp_reset: token=%u addr_id=%u (stub)\n", token, addr_id);
    spinlock_release(&mptcp_lock);
    return -EOPNOTSUPP;
}

/* ── mptcp_mp_join_syn: Build SYN with MP_JOIN option (client side) ──
 * Called when the local MPTCP layer wants to add a new subflow to an
 * existing MPTCP connection.  Builds the MP_JOIN SYN option with the
 * connection token (derived from the peer's key), a random nonce, and
 * an address ID identifying the local endpoint.
 * Returns bytes written to opt_out on success, negative errno on error. */
int mptcp_mp_join_syn(uint32_t token, uint32_t addr, uint16_t port,
                       uint8_t *opt_out, uint16_t *opt_len)
{
    (void)addr;
    (void)port;
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_mp_join_syn: not initialized\n");
        return -ENOSYS;
    }
    if (!opt_out || !opt_len) {
        kprintf("[mptcp] mptcp_mp_join_syn: NULL output pointer\n");
        return -EINVAL;
    }
    if (*opt_len < MPTCP_JOIN_SYN_LEN) {
        kprintf("[mptcp] mptcp_mp_join_syn: buffer too small (%u < %u)\n",
                *opt_len, MPTCP_JOIN_SYN_LEN);
        return -ENOSPC;
    }

    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_mp_join_syn: token %u not found\n", token);
        return -EINVAL;
    }
    if (!mc->established) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_mp_join_syn: token %u not yet established\n", token);
        return -EINVAL;
    }

    /* Derive the connection token from our sender key (this is what the
     * peer will look up) */
    uint32_t join_token = mptcp_token_from_key(mc->snd_key);

    /* Generate a random nonce using timer ticks and the MPTCP next-token counter */
    uint32_t nonce = (uint32_t)(timer_get_ticks() ^
                    ((uint64_t)timer_get_ticks() >> 16));
    nonce ^= (uint32_t)((uint64_t)token >> 12);
    nonce ^= mptcp_next_token++;

    /* Use the number of existing subflows as the addr_id */
    uint8_t addr_id = (uint8_t)mc->num_subflows;

    /* Build the MP_JOIN SYN option */
    int ret = mptcp_build_join_syn(opt_out, opt_len, addr_id, join_token, nonce);
    if (ret == 0) {
        kprintf("[MPTCP] MP_JOIN SYN built: token=%u join_token=%u "
                "nonce=%u addr_id=%u addr=%u:%u\n",
                token, join_token, nonce, (unsigned)addr_id,
                addr, (unsigned)port);
    }

    spinlock_release(&mptcp_lock);
    return ret;
}

/* ── mptcp_mp_join_synack: Build SYN+ACK with MP_JOIN option (server side) ──
 * Called when the server has received an MP_JOIN SYN and needs to respond
 * with a SYN+ACK containing the MP_JOIN option.  Retrieves the pending
 * subflow state, computes the HMAC, and builds the response option.
 * Returns bytes written to opt_out on success, negative errno on error. */
int mptcp_mp_join_synack(uint32_t token, uint32_t addr, uint16_t port,
                          uint8_t *opt_out, uint16_t *opt_len)
{
    (void)addr;
    (void)port;
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_mp_join_synack: not initialized\n");
        return -ENOSYS;
    }
    if (!opt_out || !opt_len) {
        kprintf("[mptcp] mptcp_mp_join_synack: NULL output pointer\n");
        return -EINVAL;
    }
    if (*opt_len < MPTCP_JOIN_SYNACK_LEN) {
        return -ENOSPC;
    }

    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_mp_join_synack: token %u not found\n", token);
        return -EINVAL;
    }

    /* Find the subflow with a pending MP_JOIN that has no response yet.
     * We look for a subflow whose join_local_nonce was set by handle_join
     * but whose join_hmac still matches the state from that call. */
    struct mptcp_subflow *sf = NULL;
    int slot = -1;
    for (int i = 0; i < mc->num_subflows && i < MPTCP_MAX_SUBFLOWS; i++) {
        if (mc->subflows[i].used &&
            mc->subflows[i].join_local_nonce != 0 &&
            mc->subflows[i].join_nonce != 0) {
            sf = &mc->subflows[i];
            slot = i;
            break;
        }
    }
    if (!sf) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_mp_join_synack: no pending subflow for token %u\n",
                token);
        return -ENOENT;
    }

    /* Build the SYN+ACK option with the stored HMAC and our nonce */
    int ret = mptcp_build_join_synack(opt_out, opt_len,
                                       sf->join_local_id,
                                       sf->join_local_nonce,
                                       sf->join_hmac);
    if (ret == 0) {
        kprintf("[MPTCP] MP_JOIN SYN+ACK built: token=%u slot=%d "
                "srv_nonce=%u srv_addr_id=%u\n",
                token, slot,
                sf->join_local_nonce,
                (unsigned)sf->join_local_id);
    }

    spinlock_release(&mptcp_lock);
    return ret;
}

/* ── mptcp_mp_join_ack ──────────────────
 * Called on the server side when the 3rd ACK of a MP_JOIN handshake
 * arrives.  Parses the MP_JOIN ACK option, validates the HMAC, and
 * marks the subflow as fully established.
 * Returns 0 on success (subflow established), negative errno on failure. */
int mptcp_mp_join_ack(uint32_t token, uint32_t addr_id,
                       const uint8_t *opt, uint16_t opt_len)
{
    (void)addr_id;
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_mp_join_ack: not initialized\n");
        return -ENOSYS;
    }
    if (!opt || opt_len < MPTCP_JOIN_ACK_LEN) {
        kprintf("[mptcp] mptcp_mp_join_ack: invalid option (ptr=%p len=%u)\n",
                (const void *)opt, (unsigned)opt_len);
        return -EINVAL;
    }

    /* Parse the client's HMAC from the ACK */
    uint8_t cli_hmac[8];
    int ret = mptcp_parse_join_ack(opt, opt_len, cli_hmac);
    if (ret < 0)
        return ret;

    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_mp_join_ack: token %u not found\n", token);
        return -EINVAL;
    }

    /* Find the subflow with a pending MP_JOIN that matches this ACK.
     * The client sends HMAC-SHA256(snd_key, cli_nonce || srv_nonce || cli_id || srv_id)
     * We need to compute the expected HMAC and compare. */
    int found = 0;
    for (int i = 0; i < mc->num_subflows && i < MPTCP_MAX_SUBFLOWS; i++) {
        struct mptcp_subflow *sf = &mc->subflows[i];
        if (!sf->used)
            continue;

        /* Compute the expected client HMAC:
         * key = mc->rcv_key (the peer's key, since client uses its snd_key)
         * send_nonce = cli_nonce (client's nonce = sf->join_nonce)
         * recv_nonce = srv_nonce (server's nonce = sf->join_local_nonce)
         * send_id = cli_addr_id (client's id = sf->join_id)
         * recv_id = srv_addr_id (server's id = sf->join_local_id) */
        uint8_t expected_hmac[8];
        mptcp_join_compute_hmac(mc->rcv_key,
                                sf->join_nonce,       /* client nonce */
                                sf->join_local_nonce, /* server nonce */
                                sf->join_id,          /* client addr_id */
                                sf->join_local_id,    /* server addr_id */
                                expected_hmac);

        /* Compare */
        if (memcmp(cli_hmac, expected_hmac, 8) == 0) {
            /* HMAC matches — subflow is established */
            kprintf("[MPTCP] MP_JOIN ACK validated: token=%u slot=%d "
                    "subflow established\n", token, i);
            /* The subflow's conn_id was set by mptcp_handle_join */
            found = 1;

            /* Clear join state since the handshake is complete */
            sf->join_nonce = 0;
            sf->join_local_nonce = 0;
            sf->join_id = 0;
            sf->join_local_id = 0;
            memset(sf->join_hmac, 0, 8);

            /* Mark the MPTCP connection for this subflow */
            if (sf->conn_id >= 0 && sf->conn_id < MAX_TCP_CONNS) {
                struct tcp_conn *c = &tcp_conns[sf->conn_id];
                c->mptcp_token = token;
            }

            spinlock_release(&mptcp_lock);
            return 0;
        }
    }

    if (!found) {
        kprintf("[mptcp] mptcp_mp_join_ack: token=%u no matching subflow "
                "or HMAC mismatch\n", token);
        spinlock_release(&mptcp_lock);
        return -EPERM;
    }

    spinlock_release(&mptcp_lock);
    return 0;
}

/* ── mptcp_dss: Record data sequence mapping for a send ─────────
 * Called when the MPTCP layer or a kernel module sends data on
 * an MPTCP connection.  Records the data-level sequence number
 * mapping for the outgoing segment and returns the data-level
 * sequence number (the DSN that should go in the DSS option).
 *
 * Parameters:
 *   token - MPTCP connection token
 *   seq   - subflow-level sequence number of this segment
 *   ack   - subflow-level ACK number (unused in DSS send path)
 *   data  - pointer to the data (used for length only)
 *   len   - number of bytes in the segment
 *
 * Returns: data-level sequence number (>= 0) on success,
 *          negative errno on failure. */
int mptcp_dss(uint32_t token, uint32_t seq, uint32_t ack, const void *data, uint32_t len)
{
    (void)seq;
    (void)ack;
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_dss: not initialized\n");
        return -ENOSYS;
    }
    if (data == NULL && len > 0) {
        kprintf("[mptcp] mptcp_dss: data is NULL but len=%u\n", len);
        return -EINVAL;
    }
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_dss: token %u not found\n", token);
        return -EINVAL;
    }
    if (!mc->established) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_dss: token %u not established\n", token);
        return -EINVAL;
    }

    /* Record the data-level sequence number for this segment.
     * The TCP stack will embed a DSS option with this DSN. */
    uint64_t data_seq = mc->snd_data_seq;
    mc->snd_data_seq += len;

    /* Try to find an active subflow and update its DSS mapping */
    for (uint8_t i = 0; i < mc->num_subflows; i++) {
        struct mptcp_subflow *sf = &mc->subflows[i];
        if (sf->used && !sf->backup) {
            sf->dss_data_seq = data_seq;
            sf->dss_subflow_seq = seq;  /* subflow-level seq of this segment */
            sf->dss_mapped_len = (uint16_t)len;
            break;
        }
    }

    kprintf("[MPTCP-DSS] Send mapping: token=%u data_seq=%lu len=%u\n",
            token, (unsigned long)data_seq, len);

    spinlock_release(&mptcp_lock);
    return (int)(data_seq & 0x7FFFFFFF); /* return lower 31 bits */
}

EXPORT_SYMBOL(mptcp_subflow_create);
EXPORT_SYMBOL(mptcp_add_addr);
EXPORT_SYMBOL(mptcp_remove_addr);
EXPORT_SYMBOL(mptcp_has_addr);
EXPORT_SYMBOL(mptcp_find_free_addr);
EXPORT_SYMBOL(mptcp_build_add_addr_v4);
EXPORT_SYMBOL(mptcp_build_remove_addr);
EXPORT_SYMBOL(mptcp_parse_add_addr);
EXPORT_SYMBOL(mptcp_parse_remove_addr);
EXPORT_SYMBOL(mptcp_handle_add_addr);
EXPORT_SYMBOL(mptcp_handle_remove_addr);
EXPORT_SYMBOL(mptcp_priority);
EXPORT_SYMBOL(mptcp_fastclose);
EXPORT_SYMBOL(mptcp_reset);
EXPORT_SYMBOL(mptcp_mp_join_syn);
EXPORT_SYMBOL(mptcp_mp_join_synack);
EXPORT_SYMBOL(mptcp_mp_join_ack);
EXPORT_SYMBOL(mptcp_build_dss);
EXPORT_SYMBOL(mptcp_parse_dss);
EXPORT_SYMBOL(mptcp_dss);
EXPORT_SYMBOL(mptcp_get_token);
EXPORT_SYMBOL(mptcp_associate);
EXPORT_SYMBOL(mptcp_build_capable_syn);
EXPORT_SYMBOL(mptcp_build_capable_synack);
EXPORT_SYMBOL(mptcp_build_capable_ack);
EXPORT_SYMBOL(mptcp_parse_capable);
EXPORT_SYMBOL(mptcp_token_from_key);
EXPORT_SYMBOL(mptcp_build_join_syn);
EXPORT_SYMBOL(mptcp_build_join_synack);
EXPORT_SYMBOL(mptcp_build_join_ack);
EXPORT_SYMBOL(mptcp_parse_join_syn);
EXPORT_SYMBOL(mptcp_parse_join_synack);
EXPORT_SYMBOL(mptcp_parse_join_ack);
#include "module.h"
module_init(mptcp_init);
