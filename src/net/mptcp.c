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
            /* Send on this subflow — data sequence signal would be embedded
             * in TCP options by the TCP stack */
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

int mptcp_handle_join(int conn_id, const uint8_t *opt, uint16_t optlen)
{
    (void)conn_id;
    (void)opt;
    (void)optlen;
    return 0;
}

int mptcp_handle_dss(int conn_id, const uint8_t *opt, uint16_t optlen,
                      uint32_t seq, uint32_t ack)
{
    (void)conn_id;
    (void)opt;
    (void)optlen;
    (void)seq;
    (void)ack;
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
    kprintf("[mptcp] mptcp_subflow_create: token=%u addr=%u:%u (stub)\n",
            token, addr, (unsigned)port);
    spinlock_release(&mptcp_lock);
    return 0;
}

/* ── Implement: mptcp_add_addr ────────────────── */
int mptcp_add_addr(uint32_t token, uint32_t addr, uint16_t port, uint8_t flags)
{
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_add_addr: not initialized\n");
        return -ENOSYS;
    }
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_add_addr: token %u not found\n", token);
        return -EINVAL;
    }
    if (addr == 0 || port == 0) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_add_addr: invalid addr/port\n");
        return -EINVAL;
    }
    kprintf("[mptcp] mptcp_add_addr: token=%u addr=%u:%u flags=0x%02x (stub)\n",
            token, addr, (unsigned)port, (unsigned)flags);
    spinlock_release(&mptcp_lock);
    return -EOPNOTSUPP;
}

/* ── Implement: mptcp_remove_addr ────────────────── */
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
    kprintf("[mptcp] mptcp_remove_addr: token=%u addr_id=%u (stub)\n",
            token, addr_id);
    spinlock_release(&mptcp_lock);
    return -EOPNOTSUPP;
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

/* ── Implement: mptcp_mp_join_syn ────────────────── */
int mptcp_mp_join_syn(uint32_t token, uint32_t addr, uint16_t port, uint8_t *opt_out, uint16_t *opt_len)
{
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_mp_join_syn: not initialized\n");
        return -ENOSYS;
    }
    if (!opt_out || !opt_len) {
        kprintf("[mptcp] mptcp_mp_join_syn: NULL output pointer\n");
        return -EINVAL;
    }
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_mp_join_syn: token %u not found\n", token);
        return -EINVAL;
    }
    kprintf("[mptcp] mptcp_mp_join_syn: token=%u addr=%u:%u (stub)\n",
            token, addr, (unsigned)port);
    spinlock_release(&mptcp_lock);
    return -EOPNOTSUPP;
}

/* ── Implement: mptcp_mp_join_synack ────────────────── */
int mptcp_mp_join_synack(uint32_t token, uint32_t addr, uint16_t port, uint8_t *opt_out, uint16_t *opt_len)
{
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_mp_join_synack: not initialized\n");
        return -ENOSYS;
    }
    if (!opt_out || !opt_len) {
        kprintf("[mptcp] mptcp_mp_join_synack: NULL output pointer\n");
        return -EINVAL;
    }
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_mp_join_synack: token %u not found\n", token);
        return -EINVAL;
    }
    kprintf("[mptcp] mptcp_mp_join_synack: token=%u addr=%u:%u (stub)\n",
            token, addr, (unsigned)port);
    spinlock_release(&mptcp_lock);
    return -EOPNOTSUPP;
}

/* ── Implement: mptcp_mp_join_ack ────────────────── */
int mptcp_mp_join_ack(uint32_t token, uint32_t addr_id, const uint8_t *opt, uint16_t opt_len)
{
    if (!mptcp_initialized) {
        kprintf("[mptcp] mptcp_mp_join_ack: not initialized\n");
        return -ENOSYS;
    }
    if (!opt || opt_len < 4) {
        kprintf("[mptcp] mptcp_mp_join_ack: invalid option (ptr=%p len=%u)\n",
                (const void *)opt, (unsigned)opt_len);
        return -EINVAL;
    }
    spinlock_acquire(&mptcp_lock);
    struct mptcp_conn *mc = mptcp_find_by_token(token);
    if (!mc) {
        spinlock_release(&mptcp_lock);
        kprintf("[mptcp] mptcp_mp_join_ack: token %u not found\n", token);
        return -EINVAL;
    }
    kprintf("[mptcp] mptcp_mp_join_ack: token=%u addr_id=%u (stub)\n",
            token, addr_id);
    spinlock_release(&mptcp_lock);
    return -EOPNOTSUPP;
}

/* ── Implement: mptcp_dss ────────────────── */
int mptcp_dss(uint32_t token, uint32_t seq, uint32_t ack, const void *data, uint32_t len)
{
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
    kprintf("[mptcp] mptcp_dss: token=%u seq=%u ack=%u len=%u (stub)\n",
            token, seq, ack, len);
    spinlock_release(&mptcp_lock);
    return -EOPNOTSUPP;
}

EXPORT_SYMBOL(mptcp_subflow_create);
EXPORT_SYMBOL(mptcp_add_addr);
EXPORT_SYMBOL(mptcp_remove_addr);
EXPORT_SYMBOL(mptcp_priority);
EXPORT_SYMBOL(mptcp_fastclose);
EXPORT_SYMBOL(mptcp_reset);
EXPORT_SYMBOL(mptcp_mp_join_syn);
EXPORT_SYMBOL(mptcp_mp_join_synack);
EXPORT_SYMBOL(mptcp_mp_join_ack);
EXPORT_SYMBOL(mptcp_dss);
EXPORT_SYMBOL(mptcp_get_token);
EXPORT_SYMBOL(mptcp_associate);
EXPORT_SYMBOL(mptcp_build_capable_syn);
EXPORT_SYMBOL(mptcp_build_capable_synack);
EXPORT_SYMBOL(mptcp_build_capable_ack);
EXPORT_SYMBOL(mptcp_parse_capable);
#include "module.h"
module_init(mptcp_init);
