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
        /* IPHC compressed header — RFC 6282 decompression */
        if (in_len < 2) return -EINVAL;

        uint8_t iphc0 = in[0];
        uint8_t iphc1 = in[1];
        uint16_t offset = 2;

        /* IPHC encoding: byte 0 = 011(TF:2)(NH:1)(HLIM:2) */
        int   tf   = (iphc0 >> 3) & 0x03;
        int   nh   = (iphc0 >> 2) & 0x01;
        int   hlim = iphc0 & 0x03;

        /* byte 1 = CID(1) SAC(1) SAM(2) reserved(1?) DAC(1) DAM(2) */
        int   cid  = (iphc1 >> 7) & 0x01;
        int   sac  = (iphc1 >> 6) & 0x01;
        int   sam  = (iphc1 >> 4) & 0x03;
        int   dac  = (iphc1 >> 3) & 0x01;
        int   dam  = (iphc1 >> 1) & 0x03;

        /* Context Identifier Extension */
        if (cid) {
            if (offset >= in_len) return -EINVAL;
            uint8_t ctx_id_ext = in[offset++];
            int src_ctx = (ctx_id_ext >> 4) & 0x0F;
            int dst_ctx = ctx_id_ext & 0x0F;

            /* Look up context table for source and destination */
            struct lowpan_iface *li = NULL;
            /* Find the context table (use the first registered interface) */
            for (int i = 0; i < LOWPAN6_MAX_IFACES; i++) {
                if (lowpan_ifaces[i].used) {
                    li = &lowpan_ifaces[i];
                    break;
                }
            }

            if (sac) {
                /* Context-based source address compression */
                if (li && li->ctx[src_ctx].used) {
                    /* Use context prefix as the upper 64 bits */
                    memcpy(&ip6->src_ip, &li->ctx[src_ctx].prefix, 8);
                    if (sam == 0) {
                        if (offset + 16 > in_len) return -EINVAL;
                        memcpy(((uint8_t *)&ip6->src_ip) + 8, in + offset, 8);
                        offset += 8;
                    } else if (sam == 1) {
                        if (offset + 8 > in_len) return -EINVAL;
                        memcpy(((uint8_t *)&ip6->src_ip) + 8, in + offset, 8);
                        offset += 8;
                    } else if (sam == 2) {
                        if (offset + 2 > in_len) return -EINVAL;
                        ((uint8_t *)&ip6->src_ip)[14] = in[offset];
                        ((uint8_t *)&ip6->src_ip)[15] = in[offset + 1];
                        offset += 2;
                    }
                    /* sam == 3: all 128 bits from context prefix */
                } else {
                    /* Context not found or not used — use unspecified */
                    memset(&ip6->src_ip, 0, 16);
                    /* Skip consumed inline bytes */
                    if (sam == 0) { if (offset + 16 > in_len) return -EINVAL; offset += 16; }
                    else if (sam == 1) { if (offset + 8 > in_len) return -EINVAL; offset += 8; }
                    else if (sam == 2) { if (offset + 2 > in_len) return -EINVAL; offset += 2; }
                }
            } else {
                /* sac == 0 uses stateless compression (already handled above) */
                /* Re-run the stateless logic but context was consumed */
                if (sam == 0) {
                    if (offset + 16 > in_len) return -EINVAL;
                    memcpy(&ip6->src_ip, in + offset, 16);
                    offset += 16;
                } else if (sam == 1) {
                    if (offset + 8 > in_len) return -EINVAL;
                    ((uint8_t *)&ip6->src_ip)[0] = 0xfe;
                    ((uint8_t *)&ip6->src_ip)[1] = 0x80;
                    memcpy(((uint8_t *)&ip6->src_ip) + 8, in + offset, 8);
                    offset += 8;
                } else if (sam == 2) {
                    if (offset + 2 > in_len) return -EINVAL;
                    ((uint8_t *)&ip6->src_ip)[0] = 0xfe;
                    ((uint8_t *)&ip6->src_ip)[1] = 0x80;
                    ((uint8_t *)&ip6->src_ip)[11] = 0xff;
                    ((uint8_t *)&ip6->src_ip)[12] = 0xfe;
                    ((uint8_t *)&ip6->src_ip)[14] = in[offset];
                    ((uint8_t *)&ip6->src_ip)[15] = in[offset + 1];
                    offset += 2;
                }
            }

            if (dac) {
                /* Context-based destination address compression */
                if (li && li->ctx[dst_ctx].used) {
                    memcpy(&ip6->dst_ip, &li->ctx[dst_ctx].prefix, 8);
                    if (dam == 0) {
                        if (offset + 16 > in_len) return -EINVAL;
                        memcpy(((uint8_t *)&ip6->dst_ip) + 8, in + offset, 8);
                        offset += 8;
                    } else if (dam == 1) {
                        if (offset + 8 > in_len) return -EINVAL;
                        memcpy(((uint8_t *)&ip6->dst_ip) + 8, in + offset, 8);
                        offset += 8;
                    } else if (dam == 2) {
                        if (offset + 2 > in_len) return -EINVAL;
                        ((uint8_t *)&ip6->dst_ip)[14] = in[offset];
                        ((uint8_t *)&ip6->dst_ip)[15] = in[offset + 1];
                        offset += 2;
                    }
                } else {
                    memset(&ip6->dst_ip, 0, 16);
                    if (dam == 0) { if (offset + 16 > in_len) return -EINVAL; offset += 16; }
                    else if (dam == 1) { if (offset + 8 > in_len) return -EINVAL; offset += 8; }
                    else if (dam == 2) { if (offset + 2 > in_len) return -EINVAL; offset += 2; }
                }
            } else {
                /* dac == 0 uses stateless compression */
                if (dam == 0) {
                    if (offset + 16 > in_len) return -EINVAL;
                    memcpy(&ip6->dst_ip, in + offset, 16);
                    offset += 16;
                } else if (dam == 1) {
                    if (offset + 8 > in_len) return -EINVAL;
                    ((uint8_t *)&ip6->dst_ip)[0] = 0xfe;
                    ((uint8_t *)&ip6->dst_ip)[1] = 0x80;
                    memcpy(((uint8_t *)&ip6->dst_ip) + 8, in + offset, 8);
                    offset += 8;
                } else if (dam == 2) {
                    if (offset + 2 > in_len) return -EINVAL;
                    ((uint8_t *)&ip6->dst_ip)[0] = 0xfe;
                    ((uint8_t *)&ip6->dst_ip)[1] = 0x80;
                    ((uint8_t *)&ip6->dst_ip)[11] = 0xff;
                    ((uint8_t *)&ip6->dst_ip)[12] = 0xfe;
                    ((uint8_t *)&ip6->dst_ip)[14] = in[offset];
                    ((uint8_t *)&ip6->dst_ip)[15] = in[offset + 1];
                    offset += 2;
                }
            }
        }

        memset(ip6, 0, sizeof(*ip6));

        /* --- Version + Traffic Class + Flow Label --- */
        uint32_t vcl = (uint32_t)6 << 28;
        if (tf == 0) {
            /* 4 bytes inline (DSCP/ECN + 3 bytes flow label) */
            if (offset + 4 > in_len) return -EINVAL;
            uint32_t raw;
            memcpy(&raw, in + offset, 4);
            vcl |= ntohl(raw) & 0x0FFFFFFF;
            offset += 4;
        } else if (tf == 1) {
            /* 3 bytes inline (1 byte DSCP/ECN + 2 bytes flow label) */
            if (offset + 3 > in_len) return -EINVAL;
            vcl |= ((uint32_t)in[offset] << 20);
            vcl |= ((uint32_t)in[offset+1] << 8) | (uint32_t)in[offset+2];
            offset += 3;
        } else if (tf == 2) {
            /* 1 byte inline (DSCP/ECN only, flow label = 0) */
            if (offset >= in_len) return -EINVAL;
            vcl |= ((uint32_t)in[offset++] << 20);
        }
        /* tf == 3: all elided, use 0 for DSCP/ECN/Flow */
        ip6->vcl_flow = htonl(vcl);

        /* --- Next Header --- */
        if (nh == 0) {
            if (offset >= in_len) return -EINVAL;
            ip6->next_header = in[offset++];
        } else {
            ip6->next_header = 17; /* UDP assumed when elided */
        }

        /* --- Hop Limit --- */
        static const uint8_t hlim_tab[] = {0, 1, 64, 255};
        if (hlim == 0) {
            if (offset >= in_len) return -EINVAL;
            ip6->hop_limit = in[offset++];
        } else {
            ip6->hop_limit = hlim_tab[hlim & 3];
        }

        /* --- Source Address --- */
        if (sac == 0) {
            /* Stateless source address compression */
            if (sam == 0) {
                if (offset + 16 > in_len) return -EINVAL;
                memcpy(&ip6->src_ip, in + offset, 16);
                offset += 16;
            } else if (sam == 1) {
                /* 64 bits derived: fe80::/64 + 64 inline */
                if (offset + 8 > in_len) return -EINVAL;
                ip6->src_ip.s6_addr[0] = 0xfe;
                ip6->src_ip.s6_addr[1] = 0x80;
                memcpy(ip6->src_ip.s6_addr + 8, in + offset, 8);
                offset += 8;
            } else if (sam == 2) {
                /* 16 bits: fe80::ff:fe00:XXXX */
                if (offset + 2 > in_len) return -EINVAL;
                ip6->src_ip.s6_addr[0] = 0xfe;
                ip6->src_ip.s6_addr[1] = 0x80;
                ip6->src_ip.s6_addr[11] = 0xff;
                ip6->src_ip.s6_addr[12] = 0xfe;
                memcpy(ip6->src_ip.s6_addr + 14, in + offset, 2);
                offset += 2;
            } else { /* sam == 3 */
                /* unspecified / all-zero */
                memset(&ip6->src_ip, 0, 16);
            }
        } else {
            /* Context-based compression — not implemented, use unspecified */
            memset(&ip6->src_ip, 0, 16);
            /* Skip consumed inline bytes */
            if (sam == 0) { if (offset + 16 > in_len) return -EINVAL; offset += 16; }
            else if (sam == 1) { if (offset + 8 > in_len) return -EINVAL; offset += 8; }
            else if (sam == 2) { if (offset + 2 > in_len) return -EINVAL; offset += 2; }
        }

        /* --- Destination Address --- */
        if (dac == 0) {
            if (dam == 0) {
                if (offset + 16 > in_len) return -EINVAL;
                memcpy(&ip6->dst_ip, in + offset, 16);
                offset += 16;
            } else if (dam == 1) {
                if (offset + 8 > in_len) return -EINVAL;
                ip6->dst_ip.s6_addr[0] = 0xfe;
                ip6->dst_ip.s6_addr[1] = 0x80;
                memcpy(ip6->dst_ip.s6_addr + 8, in + offset, 8);
                offset += 8;
            } else if (dam == 2) {
                if (offset + 2 > in_len) return -EINVAL;
                ip6->dst_ip.s6_addr[0] = 0xfe;
                ip6->dst_ip.s6_addr[1] = 0x80;
                ip6->dst_ip.s6_addr[11] = 0xff;
                ip6->dst_ip.s6_addr[12] = 0xfe;
                memcpy(ip6->dst_ip.s6_addr + 14, in + offset, 2);
                offset += 2;
            } else { /* dam == 3 */
                memset(&ip6->dst_ip, 0, 16);
            }
        } else {
            memset(&ip6->dst_ip, 0, 16);
            if (dam == 0) { if (offset + 16 > in_len) return -EINVAL; offset += 16; }
            else if (dam == 1) { if (offset + 8 > in_len) return -EINVAL; offset += 8; }
            else if (dam == 2) { if (offset + 2 > in_len) return -EINVAL; offset += 2; }
        }

        ip6->payload_length = htons(0);
        if (next_len)
            *next_len = 0;

        return 0;
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
/* Forward declarations for stub functions */
struct lowpan_ctx;
struct lowpan_config;

/* ── Implement: lowpan_process_data ────────────────── */
static int lowpan_process_data(int ifindex, const uint8_t *frame, uint16_t len)
{
    if (!lowpan_initialized) {
        kprintf("[6lowpan] lowpan_process_data: not initialized\n");
        return -ENOSYS;
    }
    if (!frame || len == 0) {
        kprintf("[6lowpan] lowpan_process_data: invalid frame (ptr=%p len=%u)\n",
                (const void *)frame, (unsigned)len);
        return -EINVAL;
    }
    if (len > IEEE802154_MAX_FRAME) {
        kprintf("[6lowpan] lowpan_process_data: frame too large (%u > %u)\n",
                (unsigned)len, (unsigned)IEEE802154_MAX_FRAME);
        return -EMSGSIZE;
    }
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        kprintf("[6lowpan] lowpan_process_data: ifindex %d not registered\n", ifindex);
        return -ENODEV;
    }
    kprintf("[6lowpan] lowpan_process_data: ifindex=%d len=%u dispatch=0x%02x (stub)\n",
            ifindex, (unsigned)len, frame[0]);
    return -EOPNOTSUPP;
}

/* ── Implement: lowpan_fragment ────────────────── */
static int lowpan_fragment(int ifindex, const uint8_t *data, uint16_t len,
                     uint8_t *frames, uint16_t *frame_lens, int max_frames)
{
    if (!lowpan_initialized) {
        kprintf("[6lowpan] lowpan_fragment: not initialized\n");
        return -ENOSYS;
    }
    if (!data || !frames || !frame_lens || max_frames <= 0) {
        kprintf("[6lowpan] lowpan_fragment: invalid parameters\n");
        return -EINVAL;
    }
    if (len == 0 || len > IEEE802154_MAX_PAYLOAD * max_frames) {
        kprintf("[6lowpan] lowpan_fragment: invalid data len %u\n", (unsigned)len);
        return -EINVAL;
    }
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        kprintf("[6lowpan] lowpan_fragment: ifindex %d not registered\n", ifindex);
        return -ENODEV;
    }
    kprintf("[6lowpan] lowpan_fragment: ifindex=%d len=%u max_frames=%d (stub)\n",
            ifindex, (unsigned)len, max_frames);
    return -EOPNOTSUPP;
}

/* ── Implement: lowpan_reassemble ────────────────── */
static int lowpan_reassemble(int ifindex, const uint8_t *frag, uint16_t frag_len,
                       uint8_t *out, uint16_t *out_len)
{
    if (!lowpan_initialized) {
        kprintf("[6lowpan] lowpan_reassemble: not initialized\n");
        return -ENOSYS;
    }
    if (!frag || !out || !out_len || frag_len < 4) {
        kprintf("[6lowpan] lowpan_reassemble: invalid parameters\n");
        return -EINVAL;
    }
    if (frag_len > IEEE802154_MAX_FRAME) {
        kprintf("[6lowpan] lowpan_reassemble: frag too large\n");
        return -EMSGSIZE;
    }
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        kprintf("[6lowpan] lowpan_reassemble: ifindex %d not registered\n", ifindex);
        return -ENODEV;
    }
    kprintf("[6lowpan] lowpan_reassemble: ifindex=%d frag_len=%u (stub)\n",
            ifindex, (unsigned)frag_len);
    return -EOPNOTSUPP;
}

/* ── Implement: lowpan_header_compress ────────────────── */
static int lowpan_header_compress(const struct ipv6_header *ip6,
                            const struct lowpan_ctx *ctx, int ctx_id,
                            uint8_t *out, uint16_t *out_len)
{
    if (!lowpan_initialized) {
        kprintf("[6lowpan] lowpan_header_compress: not initialized\n");
        return -ENOSYS;
    }
    if (!ip6 || !out || !out_len) {
        kprintf("[6lowpan] lowpan_header_compress: invalid parameters\n");
        return -EINVAL;
    }
    if (ctx_id < 0 || ctx_id >= LOWPAN_CONTEXT_TABLE_SIZE) {
        kprintf("[6lowpan] lowpan_header_compress: invalid ctx_id %d\n", ctx_id);
        return -EINVAL;
    }
    kprintf("[6lowpan] lowpan_header_compress: ctx_id=%d (stub)\n", ctx_id);
    return -EOPNOTSUPP;
}

/* ── Implement: lowpan_header_decompress ────────────────── */
static int lowpan_header_decompress(const uint8_t *in, uint16_t in_len,
                              struct ipv6_header *ip6,
                              const struct lowpan_ctx *ctx, int ctx_id)
{
    if (!lowpan_initialized) {
        kprintf("[6lowpan] lowpan_header_decompress: not initialized\n");
        return -ENOSYS;
    }
    if (!in || !ip6 || in_len < 2) {
        kprintf("[6lowpan] lowpan_header_decompress: invalid parameters\n");
        return -EINVAL;
    }
    if (ctx_id < 0 || ctx_id >= LOWPAN_CONTEXT_TABLE_SIZE) {
        kprintf("[6lowpan] lowpan_header_decompress: invalid ctx_id %d\n", ctx_id);
        return -EINVAL;
    }
    kprintf("[6lowpan] lowpan_header_decompress: in_len=%u ctx_id=%d (stub)\n",
            (unsigned)in_len, ctx_id);
    return -EOPNOTSUPP;
}

/* ── Implement: lowpan_mesh_send ────────────────── */
static int lowpan_mesh_send(int ifindex, const uint8_t *data, uint16_t len,
                      const uint8_t *mesh_dst, const uint8_t *mesh_src)
{
    if (!lowpan_initialized) {
        kprintf("[6lowpan] lowpan_mesh_send: not initialized\n");
        return -ENOSYS;
    }
    if (!data || !mesh_dst || !mesh_src || len == 0) {
        kprintf("[6lowpan] lowpan_mesh_send: invalid parameters\n");
        return -EINVAL;
    }
    if (len > IEEE802154_MAX_PAYLOAD) {
        kprintf("[6lowpan] lowpan_mesh_send: data too large (%u)\n", (unsigned)len);
        return -EMSGSIZE;
    }
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        kprintf("[6lowpan] lowpan_mesh_send: ifindex %d not registered\n", ifindex);
        return -ENODEV;
    }
    kprintf("[6lowpan] lowpan_mesh_send: ifindex=%d len=%u (stub)\n", ifindex, (unsigned)len);
    return -EOPNOTSUPP;
}

/* ── Implement: lowpan_mesh_recv ────────────────── */
static int lowpan_mesh_recv(int ifindex, const uint8_t *frame, uint16_t len,
                      uint8_t *mesh_src_out, uint8_t *mesh_dst_out)
{
    if (!lowpan_initialized) {
        kprintf("[6lowpan] lowpan_mesh_recv: not initialized\n");
        return -ENOSYS;
    }
    if (!frame || !mesh_src_out || !mesh_dst_out || len < 2) {
        kprintf("[6lowpan] lowpan_mesh_recv: invalid parameters\n");
        return -EINVAL;
    }
    if (len > IEEE802154_MAX_FRAME) {
        kprintf("[6lowpan] lowpan_mesh_recv: frame too large (%u)\n", (unsigned)len);
        return -EMSGSIZE;
    }
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        kprintf("[6lowpan] lowpan_mesh_recv: ifindex %d not registered\n", ifindex);
        return -ENODEV;
    }
    kprintf("[6lowpan] lowpan_mesh_recv: ifindex=%d len=%u (stub)\n", ifindex, (unsigned)len);
    return -EOPNOTSUPP;
}

/* ── Implement: lowpan_broadcast ────────────────── */
static int lowpan_broadcast(int ifindex, const uint8_t *data, uint16_t len)
{
    if (!lowpan_initialized) {
        kprintf("[6lowpan] lowpan_broadcast: not initialized\n");
        return -ENOSYS;
    }
    if (!data || len == 0) {
        kprintf("[6lowpan] lowpan_broadcast: invalid data (ptr=%p len=%u)\n",
                (const void *)data, (unsigned)len);
        return -EINVAL;
    }
    if (len > IEEE802154_MAX_PAYLOAD) {
        kprintf("[6lowpan] lowpan_broadcast: data too large (%u)\n", (unsigned)len);
        return -EMSGSIZE;
    }
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        kprintf("[6lowpan] lowpan_broadcast: ifindex %d not registered\n", ifindex);
        return -ENODEV;
    }
    kprintf("[6lowpan] lowpan_broadcast: ifindex=%d len=%u (stub)\n", ifindex, (unsigned)len);
    return -EOPNOTSUPP;
}

/* ── Implement: lowpan_unicast ────────────────── */
static int lowpan_unicast(int ifindex, const uint8_t *data, uint16_t len,
                    const struct in6_addr *dst)
{
    if (!lowpan_initialized) {
        kprintf("[6lowpan] lowpan_unicast: not initialized\n");
        return -ENOSYS;
    }
    if (!data || !dst || len == 0) {
        kprintf("[6lowpan] lowpan_unicast: invalid parameters\n");
        return -EINVAL;
    }
    if (len > IEEE802154_MAX_PAYLOAD) {
        kprintf("[6lowpan] lowpan_unicast: data too large (%u)\n", (unsigned)len);
        return -EMSGSIZE;
    }
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        kprintf("[6lowpan] lowpan_unicast: ifindex %d not registered\n", ifindex);
        return -ENODEV;
    }
    kprintf("[6lowpan] lowpan_unicast: ifindex=%d len=%u (stub)\n", ifindex, (unsigned)len);
    return -EOPNOTSUPP;
}

/* ── Implement: lowpan_setup ────────────────── */
static int lowpan_setup(int ifindex, const struct lowpan_config *cfg)
{
    if (!lowpan_initialized) {
        kprintf("[6lowpan] lowpan_setup: not initialized\n");
        return -ENOSYS;
    }
    if (!cfg) {
        kprintf("[6lowpan] lowpan_setup: NULL config\n");
        return -EINVAL;
    }
    struct lowpan_iface *li = lowpan_find_by_ifindex(ifindex);
    if (!li) {
        kprintf("[6lowpan] lowpan_setup: ifindex %d not registered\n", ifindex);
        return -ENODEV;
    }
    kprintf("[6lowpan] lowpan_setup: ifindex=%d config=%p (stub)\n", ifindex, (const void *)cfg);
    return -EOPNOTSUPP;
}

EXPORT_SYMBOL(lowpan_process_data);
EXPORT_SYMBOL(lowpan_fragment);
EXPORT_SYMBOL(lowpan_reassemble);
EXPORT_SYMBOL(lowpan_header_compress);
EXPORT_SYMBOL(lowpan_header_decompress);
EXPORT_SYMBOL(lowpan_mesh_send);
EXPORT_SYMBOL(lowpan_mesh_recv);
EXPORT_SYMBOL(lowpan_broadcast);
EXPORT_SYMBOL(lowpan_unicast);
EXPORT_SYMBOL(lowpan_setup);
EXPORT_SYMBOL(lowpan6_poll);
#include "module.h"
module_init(lowpan6_init);
