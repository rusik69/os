/* ipoib.c — IP over InfiniBand (RFC 4391/4392) */

#include "ipoib.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"

#define IPOIB_MAX_IFACES 4

static struct ipoib_iface ipoib_ifaces[IPOIB_MAX_IFACES];
static spinlock_t ipoib_lock;
static int ipoib_initialized = 0;
static int ipoib_present = 0;

void ipoib_init(void)
{
    if (ipoib_initialized) return;
    spinlock_init(&ipoib_lock);
    memset(ipoib_ifaces, 0, sizeof(ipoib_ifaces));
    ipoib_initialized = 1;
    kprintf("[OK] IPoIB: IP over InfiniBand initialized\n");
}

static struct ipoib_iface *ipoib_find_by_ifindex(int ifindex)
{
    for (int i = 0; i < IPOIB_MAX_IFACES; i++) {
        if (ipoib_ifaces[i].used && ipoib_ifaces[i].ifindex == ifindex)
            return &ipoib_ifaces[i];
    }
    return NULL;
}

static int ipoib_find_free(void)
{
    for (int i = 0; i < IPOIB_MAX_IFACES; i++) {
        if (!ipoib_ifaces[i].used)
            return i;
    }
    return -1;
}

int ipoib_register_iface(int ifindex, const uint8_t *hw_addr, int mtu)
{
    spinlock_acquire(&ipoib_lock);
    int slot = ipoib_find_free();
    if (slot < 0) {
        spinlock_release(&ipoib_lock);
        return -ENOMEM;
    }

    struct ipoib_iface *ii = &ipoib_ifaces[slot];
    memset(ii, 0, sizeof(*ii));
    ii->used = 1;
    ii->ifindex = ifindex;
    if (hw_addr)
        memcpy(ii->hw_addr, hw_addr, 20);
    ii->mtu = (mtu > 0) ? mtu : 2048;
    ii->pkey = 0xFFFF;  /* Default PKey: all full members */
    ii->snd_head = ii->snd_tail = 0;
    ii->rcv_head = ii->rcv_tail = 0;

    ipoib_present = 1;
    spinlock_release(&ipoib_lock);
    return 0;
}

int ipoib_unregister_iface(int ifindex)
{
    spinlock_acquire(&ipoib_lock);
    struct ipoib_iface *ii = ipoib_find_by_ifindex(ifindex);
    if (!ii) {
        spinlock_release(&ipoib_lock);
        return -ENOENT;
    }
    memset(ii, 0, sizeof(*ii));
    /* Check if any interfaces remain */
    ipoib_present = 0;
    for (int i = 0; i < IPOIB_MAX_IFACES; i++) {
        if (ipoib_ifaces[i].used) {
            ipoib_present = 1;
            break;
        }
    }
    spinlock_release(&ipoib_lock);
    return 0;
}

int ipoib_send(int ifindex, const uint8_t *dst_gid, uint16_t type,
                const void *data, uint16_t len)
{
    (void)dst_gid;
    spinlock_acquire(&ipoib_lock);
    struct ipoib_iface *ii = ipoib_find_by_ifindex(ifindex);
    if (!ii) {
        spinlock_release(&ipoib_lock);
        return -ENODEV;
    }

    if (len > ii->mtu) {
        spinlock_release(&ipoib_lock);
        return -EMSGSIZE;
    }

    /* Build IPoIB frame: 4-byte header + payload */
    uint8_t frame[2048 + 4];
    struct ipoib_header *ih = (struct ipoib_header *)frame;
    ih->type = htons(type);
    memset(ih->reserved, 0, 2);
    memcpy(frame + sizeof(*ih), data, len);

    uint16_t frame_len = sizeof(*ih) + len;

    /* Queue for transmit */
    int next = (ii->snd_head + 1) % IPOIB_MAX_QUEUE;
    if (next == ii->snd_tail) {
        ii->tx_dropped++;
        spinlock_release(&ipoib_lock);
        return -ENOBUFS;
    }

    memcpy(ii->sndbuf[ii->snd_head], frame, frame_len);
    ii->sndlen[ii->snd_head] = frame_len;
    ii->snd_head = next;
    ii->tx_packets++;
    ii->tx_bytes += frame_len;

    spinlock_release(&ipoib_lock);
    return len;
}

int ipoib_recv(int ifindex, uint8_t *buf, uint16_t maxlen,
                uint16_t *type_out)
{
    spinlock_acquire(&ipoib_lock);
    struct ipoib_iface *ii = ipoib_find_by_ifindex(ifindex);
    if (!ii) {
        spinlock_release(&ipoib_lock);
        return -ENODEV;
    }

    if (ii->rcv_head == ii->rcv_tail) {
        spinlock_release(&ipoib_lock);
        return -EAGAIN;
    }

    uint16_t frame_len = ii->rcvlen[ii->rcv_tail];
    if (frame_len < sizeof(struct ipoib_header)) {
        ii->rcv_tail = (ii->rcv_tail + 1) % IPOIB_MAX_QUEUE;
        spinlock_release(&ipoib_lock);
        return -EINVAL;
    }

    const struct ipoib_header *ih = (const struct ipoib_header *)ii->rcvbuf[ii->rcv_tail];
    uint16_t data_len = (uint16_t)(frame_len - sizeof(*ih));
    if (data_len > maxlen) data_len = maxlen;

    memcpy(buf, ii->rcvbuf[ii->rcv_tail] + sizeof(*ih), data_len);
    if (type_out)
        *type_out = ntohs(ih->type);

    ii->rcv_tail = (ii->rcv_tail + 1) % IPOIB_MAX_QUEUE;
    ii->rx_bytes += data_len;

    spinlock_release(&ipoib_lock);
    return data_len;
}

int ipoib_is_present(void)
{
    return ipoib_present;
}

void ipoib_poll(void)
{
    /* Process send queue — in a real implementation, this would
     * interact with the InfiniBand HCA driver to actually transmit packets.
     * For now, consume the send queue to avoid buildup. */
    spinlock_acquire(&ipoib_lock);
    for (int i = 0; i < IPOIB_MAX_IFACES; i++) {
        if (!ipoib_ifaces[i].used) continue;
        struct ipoib_iface *ii = &ipoib_ifaces[i];
        ii->snd_head = ii->snd_tail;  /* Discard all queued sends */
    }
    spinlock_release(&ipoib_lock);
}

EXPORT_SYMBOL(ipoib_init);
EXPORT_SYMBOL(ipoib_register_iface);
EXPORT_SYMBOL(ipoib_unregister_iface);
EXPORT_SYMBOL(ipoib_send);
EXPORT_SYMBOL(ipoib_recv);
EXPORT_SYMBOL(ipoib_is_present);
EXPORT_SYMBOL(ipoib_poll);
#include "module.h"
module_init(ipoib_init);

/* ── Implement: ipoib_open ────────────────── */
static int ipoib_open(void *dev)
{
    if (!dev) {
        kprintf("[ipoib] ipoib_open: NULL dev\n");
        return -EINVAL;
    }
    kprintf("[ipoib] ipoib_open: dev=%p (stub)\n", dev);
    return -EOPNOTSUPP;
}
/* ── Implement: ipoib_stop ────────────────── */
static int ipoib_stop(void *dev)
{
    if (!dev) {
        kprintf("[ipoib] ipoib_stop: NULL dev\n");
        return -EINVAL;
    }
    kprintf("[ipoib] ipoib_stop: dev=%p (stub)\n", dev);
    return -EOPNOTSUPP;
}
/* ── Implement: ipoib_xmit ────────────────── */
static int ipoib_xmit(void *skb, void *dev)
{
    if (!skb || !dev) {
        kprintf("[ipoib] ipoib_xmit: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[ipoib] ipoib_xmit: skb=%p dev=%p (stub)\n", skb, dev);
    return -EOPNOTSUPP;
}
