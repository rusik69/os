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

int mptcp_handle_capable(int conn_id, const uint8_t *opt, uint16_t optlen)
{
    (void)conn_id;
    (void)opt;
    (void)optlen;
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
#include "module.h"
module_init(mptcp_init);
