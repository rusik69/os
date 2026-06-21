/* gre.c — Generic Routing Encapsulation (RFC 2784) tunneling
 *
 * Implements GRE tunnel encapsulation/decapsulation as a reusable
 * kernel subsystem.  GRE wraps arbitrary protocol packets inside IP
 * datagrams using protocol number 47.
 *
 * Supports multiple tunnels via a hash table indexed by (local_ip, remote_ip).
 *
 * Reference: RFC 2784 (GRE), RFC 2890 (GRE extensions)
 */

#define KERNEL_INTERNAL
#include "gre.h"
#include "net.h"
#include "printf.h"
#include "string.h"
#include "errno.h"

/* ── Tunnel hash table ─────────────────────────────────────────────── */

#define GRE_TUNNEL_HASH_SIZE 16
#define GRE_MAX_TUNNELS      32

static struct gre_tunnel *gre_tunnel_hash[GRE_TUNNEL_HASH_SIZE];
static struct gre_tunnel gre_tunnel_pool[GRE_MAX_TUNNELS];
static int gre_initialized = 0;

/* Simple hash function for tunnel lookup */
static uint32_t gre_tunnel_hash_key(uint32_t local, uint32_t remote)
{
    return (local ^ remote ^ (local >> 16) ^ (remote >> 16)) % GRE_TUNNEL_HASH_SIZE;
}

/* ── Initialization / Teardown ─────────────────────────────────────── */

int gre_init(void)
{
    if (gre_initialized)
        return 0;

    memset(gre_tunnel_hash, 0, sizeof(gre_tunnel_hash));
    memset(gre_tunnel_pool, 0, sizeof(gre_tunnel_pool));
    gre_initialized = 1;
    kprintf("[OK] GRE tunnel initialized (max %d tunnels, hash table)\n",
            GRE_MAX_TUNNELS);
    return 0;
}

void gre_exit(void)
{
    if (!gre_initialized)
        return;

    for (int i = 0; i < GRE_MAX_TUNNELS; i++) {
        if (gre_tunnel_pool[i].active) {
            gre_tunnel_pool[i].active = 0;
        }
    }
    gre_initialized = 0;
    kprintf("[OK] GRE tunnel shut down\n");
}

/* ── Tunnel lifecycle ──────────────────────────────────────────────── */

static struct gre_tunnel *gre_tunnel_lookup(uint32_t local, uint32_t remote)
{
    uint32_t h = gre_tunnel_hash_key(local, remote);
    struct gre_tunnel *t = gre_tunnel_hash[h];
    while (t) {
        if (t->active && t->local_ip == local && t->remote_ip == remote)
            return t;
        t = t->next;
    }
    return NULL;
}

int gre_create_tunnel(uint32_t remote, uint32_t local)
{
    if (!gre_initialized)
        return -1;

    /* Check for duplicate */
    if (gre_tunnel_lookup(local, remote))
        return -1; /* Already exists */

    /* Find free slot */
    int idx = -1;
    for (int i = 0; i < GRE_MAX_TUNNELS; i++) {
        if (!gre_tunnel_pool[i].active) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1; /* No free slots */

    struct gre_tunnel *t = &gre_tunnel_pool[idx];
    t->remote_ip = remote;
    t->local_ip  = local;
    t->active    = 1;
    t->index     = idx;

    /* Add to hash table */
    uint32_t h = gre_tunnel_hash_key(local, remote);
    t->next = gre_tunnel_hash[h];
    gre_tunnel_hash[h] = t;

    kprintf("[GRE] Tunnel %d created: local %d.%d.%d.%d -> remote %d.%d.%d.%d\n",
            idx,
            (uint8_t)(local  >> 24), (uint8_t)(local  >> 16),
            (uint8_t)(local  >> 8),  (uint8_t)(local),
            (uint8_t)(remote >> 24), (uint8_t)(remote >> 16),
            (uint8_t)(remote >> 8),  (uint8_t)(remote));
    return idx;
}

int gre_destroy_tunnel(int index)
{
    if (!gre_initialized)
        return -1;
    if (index < 0 || index >= GRE_MAX_TUNNELS)
        return -1;
    if (!gre_tunnel_pool[index].active)
        return -1;

    struct gre_tunnel *t = &gre_tunnel_pool[index];
    uint32_t h = gre_tunnel_hash_key(t->local_ip, t->remote_ip);

    /* Remove from hash table */
    struct gre_tunnel **pp = &gre_tunnel_hash[h];
    while (*pp) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
        pp = &(*pp)->next;
    }

    t->active = 0;
    t->next = NULL;
    kprintf("[GRE] Tunnel %d destroyed\n", index);
    return 0;
}

/* Find a tunnel by index */
struct gre_tunnel *gre_get_tunnel(int index)
{
    if (!gre_initialized) return NULL;
    if (index < 0 || index >= GRE_MAX_TUNNELS) return NULL;
    if (!gre_tunnel_pool[index].active) return NULL;
    return &gre_tunnel_pool[index];
}

/* ── Encapsulation ────────────────────────────────────────────────────
 *
 * Builds: [ Outer IP header ][ GRE header ][ Inner packet ]
 *
 * The GRE header used here is the basic 4-byte version:
 *   - flags = 0 (no checksum, key, or sequence number)
 *   - protocol_type = 0x0800 (EtherType for IPv4)
 */

int gre_encapsulate(int tunnel_index, const uint8_t *inner_pkt, int inner_len,
                    uint8_t *outer_buf, int outer_max)
{
    if (!gre_initialized || tunnel_index < 0)
        return -1;
    if (tunnel_index >= GRE_MAX_TUNNELS || !gre_tunnel_pool[tunnel_index].active)
        return -1;
    if (!inner_pkt || !outer_buf)
        return -1;
    if (inner_len < 20)
        return -1;  /* need at least an IP header inside */

    struct gre_tunnel *t = &gre_tunnel_pool[tunnel_index];

    /* Calculate total size: outer IP header + GRE header + inner packet */
    int total = (int)sizeof(struct ip_header)
              + (int)sizeof(struct gre_header)
              + inner_len;

    if (total > outer_max)
        return -1;

    /* ── Build outer IP header ── */
    struct ip_header *outer = (struct ip_header *)outer_buf;
    memset(outer, 0, sizeof(struct ip_header));
    outer->version_ihl = 0x45;              /* IPv4, 5 words */
    outer->ttl         = 64;
    outer->protocol    = GRE_PROTOCOL;       /* 47 = GRE */
    outer->src_ip      = htonl(t->local_ip);
    outer->dst_ip      = htonl(t->remote_ip);
    outer->total_len   = htons((uint16_t)total);
    outer->id          = htons(1);
    outer->checksum    = 0;
    outer->checksum    = net_checksum(outer, sizeof(struct ip_header));

    /* ── Build GRE header ── */
    struct gre_header *gre = (struct gre_header *)(outer_buf + sizeof(struct ip_header));
    gre->flags          = htons(GRE_VERSION_0);  /* no optional fields */
    gre->protocol_type  = htons(GRE_ETH_TYPE_IP); /* inner is IPv4 */

    /* ── Copy inner packet ── */
    memcpy(outer_buf + sizeof(struct ip_header) + sizeof(struct gre_header),
           inner_pkt, inner_len);

    return total;
}

/* ── Decapsulation ────────────────────────────────────────────────────
 *
 * Extracts the inner packet from a received GRE+IP frame.
 */

int gre_decapsulate(const uint8_t *outer_pkt, int outer_len,
                    uint8_t *inner_buf, int inner_max,
                    uint32_t *src_ip, uint32_t *dst_ip)
{
    if (!gre_initialized || !outer_pkt || !inner_buf)
        return -1;

    int min_len = (int)(sizeof(struct ip_header) + sizeof(struct gre_header));
    if (outer_len < min_len)
        return -1;

    const struct ip_header *outer = (const struct ip_header *)outer_pkt;
    if (outer->protocol != GRE_PROTOCOL)
        return -1;

    int ihl = (outer->version_ihl & 0x0F) * 4;
    if (ihl < 20 || ihl > outer_len)
        return -1;

    const struct gre_header *gre =
        (const struct gre_header *)(outer_pkt + ihl);

    uint16_t gre_flags = ntohs(gre->flags);
    if ((gre_flags & GRE_FLAG_VERSION) != GRE_VERSION_0)
        return -1;

    int gre_hdr_size = (int)sizeof(struct gre_header);
    if (gre_flags & GRE_FLAG_C) gre_hdr_size += 4;
    if (gre_flags & GRE_FLAG_K) gre_hdr_size += 4;
    if (gre_flags & GRE_FLAG_S) gre_hdr_size += 4;

    if (ihl + gre_hdr_size > outer_len)
        return -1;

    const uint8_t *inner_start = outer_pkt + ihl + gre_hdr_size;
    int inner_len = outer_len - ihl - gre_hdr_size;
    if (inner_len < 0) return -1;
    if (inner_len > inner_max) inner_len = inner_max;

    uint16_t proto = ntohs(gre->protocol_type);
    if (proto != GRE_ETH_TYPE_IP)
        return -1;

    memcpy(inner_buf, inner_start, inner_len);

    /* Return tunnel endpoint addresses */
    if (src_ip) *src_ip = ntohl(outer->src_ip);
    if (dst_ip) *dst_ip = ntohl(outer->dst_ip);

    return inner_len;
}

/* ── Module hooks (for loadable module support) ───────────────────── */

#ifdef MODULE
#include "export.h"
#include "module.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("GRE tunnel protocol (RFC 2784) - loadable kernel module");
MODULE_ALIAS("net-gre");

int init_module(void)
{
    return gre_init();
}

void cleanup_module(void)
{
    gre_exit();
}

EXPORT_SYMBOL(gre_create_tunnel);
EXPORT_SYMBOL(gre_destroy_tunnel);
EXPORT_SYMBOL(gre_get_tunnel);
EXPORT_SYMBOL(gre_encapsulate);
EXPORT_SYMBOL(gre_decapsulate);
#endif /* MODULE */

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: gre_xmit ────────────────────────────────── */
int gre_xmit(void *skb, void *dev)
{
    (void)skb;
    (void)dev;
    kprintf("[GRE] gre_xmit: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: gre_rcv ─────────────────────────────────── */
int gre_rcv(void *skb)
{
    (void)skb;
    kprintf("[GRE] gre_rcv: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: gre_err ─────────────────────────────────── */
void gre_err(void *skb, uint32_t info)
{
    (void)skb;
    (void)info;
    kprintf("[GRE] gre_err: not yet implemented\n");
}
