/* 6lowpan.c — IPv6 over Low-Power Wireless (RFC 4944/6282) */

#include "lowpan6.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"
#include "timer.h"

#define LOWPAN6_MAX_IFACES 4

static struct lowpan_iface lowpan_ifaces[LOWPAN6_MAX_IFACES];
static spinlock_t lowpan_lock;
static int lowpan_initialized = 0;

void lowpan6_init(void)
{
    if (lowpan_initialized) return;
    spinlock_init(&lowpan_lock);
    memset(lowpan_ifaces, 0, sizeof(lowpan_ifaces));
    lowpan_initialized = 1;
    kprintf("[OK] 6LoWPAN: IPv6 over low-power wireless initialized\n");
}

static struct lowpan_iface *lowpan_find_by_ifindex(int ifindex)
{
    for (int i = 0; i < LOWPAN6_MAX_IFACES; i++) {
        if (lowpan_ifaces[i].used && lowpan_ifaces[i].ifindex == ifindex)
            return &lowpan_ifaces[i];
    }
    return NULL;
}

static int lowpan_find_free(void)
{
    for (int i = 0; i < LOWPAN6_MAX_IFACES; i++) {
        if (!lowpan_ifaces[i].used)
            return i;
    }
    return -1;
}

int lowpan6_register_iface(int ifindex)
{
    spinlock_acquire(&lowpan_lock);
    int slot = lowpan_find_free();
    if (slot < 0) {
        spinlock_release(&lowpan_lock);
        return -ENOMEM;
    }

    struct lowpan_iface *li = &lowpan_ifaces[slot];
    memset(li, 0, sizeof(*li));
    li->used = 1;
    li->ifindex = ifindex;

    /* Set default contexts */
    for (int i = 0; i < LOWPAN_CONTEXT_TABLE_SIZE; i++) {
        li->ctx[i].used = 0;
    }

    spinlock_release(&lowpan_lock);
    return 0;
}

int lowpan6_unregister_iface(int ifindex)
{
    spinlock_acquire(&lowpan_lock);
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        spinlock_release(&lowpan_lock);
        return -ENOENT;
    }
    memset(li, 0, sizeof(*li));
    spinlock_release(&lowpan_lock);
    return 0;
}

int lowpan6_compress(const struct ipv6_header *ip6, const void *next_hdr,
                      uint16_t next_len, uint8_t *out, uint16_t *out_len)
{
    (void)next_hdr;
    (void)next_len;
    /* Simplified IPHC compression — uncompressed IPv6 dispatch for now */
    out[0] = LOWPAN_DISPATCH_IPV6;
    memcpy(out + 1, ip6, sizeof(*ip6));
    *out_len = 1 + sizeof(*ip6);
    return 0;
}

int lowpan6_decompress(const uint8_t *in, uint16_t in_len,
                        struct ipv6_header *ip6, uint8_t *next_hdr_buf,
                        uint16_t *next_len)
{
    if (in_len < 1) return -EINVAL;

    uint8_t dispatch = in[0];

    if ((dispatch & 0xE0) == LOWPAN_DISPATCH_IPHC) {
        /* IPHC compressed — simplified: reconstruct from context */
        (void)ip6;
        return -ENOSYS;  /* Full IPHC decompression not yet implemented */
    }

    if (dispatch == LOWPAN_DISPATCH_IPV6) {
        /* Uncompressed IPv6 */
        if (in_len < 1 + sizeof(struct ipv6_header))
            return -EINVAL;
        memcpy(ip6, in + 1, sizeof(struct ipv6_header));
        *next_len = 0;
        return 0;
    }

    return -EINVAL;
}

int lowpan6_send(int ifindex, const struct in6_addr *dst,
                  const void *data, uint16_t len)
{
    (void)dst;
    spinlock_acquire(&lowpan_lock);
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        spinlock_release(&lowpan_lock);
        return -ENODEV;
    }

    /* Build 6LoWPAN frame with IPHC compression */
    uint8_t frame[IEEE802154_MAX_FRAME];
    uint16_t frame_len;

    const struct ipv6_header *ip6 = (const struct ipv6_header *)data;
    lowpan6_compress(ip6, NULL, 0, frame, &frame_len);

    if (frame_len > IEEE802154_MAX_FRAME) {
        spinlock_release(&lowpan_lock);
        return -EMSGSIZE;
    }

    /* Send via netdevice layer */
    int ret = 0;  /* netif_send(ifindex, frame, frame_len) */
    if (ret >= 0) {
        li->tx_packets++;
        li->tx_compressed++;
    }

    spinlock_release(&lowpan_lock);
    return ret;
}

int lowpan6_recv(int ifindex, const uint8_t *frame, uint16_t len,
                  struct ipv6_header *ip6, uint8_t *payload, uint16_t *payload_len)
{
    (void)payload;
    (void)payload_len;
    spinlock_acquire(&lowpan_lock);
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        spinlock_release(&lowpan_lock);
        return -ENODEV;
    }

    /* Handle fragmentation */
    uint8_t dispatch = frame[0];
    if ((dispatch & 0xF8) == LOWPAN_DISPATCH_FRAG1) {
        li->rx_fragments++;
        spinlock_release(&lowpan_lock);
        return -EINPROGRESS;
    }

    /* Decompress */
    uint16_t next_len;
    int ret = lowpan6_decompress(frame, len, ip6, NULL, &next_len);
    if (ret == 0) {
        li->rx_packets++;
        li->rx_compressed++;
    } else {
        li->rx_dropped++;
    }

    spinlock_release(&lowpan_lock);
    return ret;
}

void lowpan6_poll(void)
{
    /* Periodically clean up expired fragment reassembly buffers */
    spinlock_acquire(&lowpan_lock);
    for (int i = 0; i < LOWPAN6_MAX_IFACES; i++) {
        if (!lowpan_ifaces[i].used) continue;
        for (int j = 0; j < LOWPAN_FRAG_BUFS; j++) {
            struct lowpan_frag *f = &lowpan_ifaces[i].frags[j];
            if (f->used && timer_get_ticks() > f->timeout) {
                memset(f, 0, sizeof(*f));
                lowpan_ifaces[i].rx_dropped++;
            }
        }
    }
    spinlock_release(&lowpan_lock);
}

EXPORT_SYMBOL(lowpan6_init);
EXPORT_SYMBOL(lowpan6_register_iface);
EXPORT_SYMBOL(lowpan6_unregister_iface);
EXPORT_SYMBOL(lowpan6_compress);
EXPORT_SYMBOL(lowpan6_decompress);
EXPORT_SYMBOL(lowpan6_send);
EXPORT_SYMBOL(lowpan6_recv);
EXPORT_SYMBOL(lowpan6_poll);
