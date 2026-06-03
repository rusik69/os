/* gre.c — Generic Routing Encapsulation (RFC 2784) tunneling
 *
 * Implements GRE tunnel encapsulation/decapsulation as a reusable
 * kernel subsystem.  GRE wraps arbitrary protocol packets inside IP
 * datagrams using protocol number 47.
 *
 * Reference: RFC 2784 (GRE), RFC 2890 (GRE extensions)
 */

#define KERNEL_INTERNAL
#include "gre.h"
#include "net.h"
#include "printf.h"
#include "string.h"

/* ── Static state ──────────────────────────────────────────────────── */

static struct gre_tunnel g_gre_tunnel;
static int gre_initialized = 0;

/* ── Initialization / Teardown ─────────────────────────────────────── */

int gre_init(void)
{
    if (gre_initialized)
        return 0;

    memset(&g_gre_tunnel, 0, sizeof(g_gre_tunnel));
    gre_initialized = 1;
    kprintf("[OK] GRE tunnel initialized (protocol %d)\n", GRE_PROTOCOL);
    return 0;
}

void gre_exit(void)
{
    if (!gre_initialized)
        return;

    g_gre_tunnel.active = 0;
    gre_initialized = 0;
    kprintf("[OK] GRE tunnel shut down\n");
}

/* ── Tunnel lifecycle ──────────────────────────────────────────────── */

int gre_create_tunnel(uint32_t remote, uint32_t local)
{
    if (!gre_initialized)
        return -1;
    if (g_gre_tunnel.active)
        return -1;  /* only one tunnel at a time for now */

    g_gre_tunnel.remote_ip = remote;
    g_gre_tunnel.local_ip  = local;
    g_gre_tunnel.active    = 1;

    kprintf("[GRE] Tunnel created: local %d.%d.%d.%d -> remote %d.%d.%d.%d\n",
            (uint8_t)(local  >> 24), (uint8_t)(local  >> 16),
            (uint8_t)(local  >> 8),  (uint8_t)(local),
            (uint8_t)(remote >> 24), (uint8_t)(remote >> 16),
            (uint8_t)(remote >> 8),  (uint8_t)(remote));
    return 0;
}

int gre_destroy_tunnel(void)
{
    if (!gre_initialized || !g_gre_tunnel.active)
        return -1;

    g_gre_tunnel.active = 0;
    kprintf("[GRE] Tunnel destroyed\n");
    return 0;
}

/* ── Encapsulation ────────────────────────────────────────────────────
 *
 * Builds: [ Outer IP header ][ GRE header ][ Inner packet ]
 *
 * The GRE header used here is the basic 4-byte version:
 *   - flags = 0 (no checksum, key, or sequence number)
 *   - protocol_type = 0x0800 (EtherType for IPv4)
 *
 * If the caller needs optional fields (checksum, key, seq#), they can
 * set the GRE_FLAG_* bits in the header after the call; this function
 * provides the minimal valid header.
 */

int gre_encapsulate(const uint8_t *inner_pkt, int inner_len,
                    uint8_t *outer_buf, int outer_max)
{
    if (!gre_initialized || !g_gre_tunnel.active)
        return -1;
    if (!inner_pkt || !outer_buf)
        return -1;
    if (inner_len < 20)
        return -1;  /* need at least an IP header inside */

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
    outer->src_ip      = htonl(g_gre_tunnel.local_ip);
    outer->dst_ip      = htonl(g_gre_tunnel.remote_ip);
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
 * Validates:
 *   1. Outer IP protocol field = 47 (GRE)
 *   2. GRE header is well-formed (version = 0, no unsupported flags)
 *   3. Inner packet fits in the output buffer
 */

int gre_decapsulate(const uint8_t *outer_pkt, int outer_len,
                    uint8_t *inner_buf, int inner_max)
{
    if (!gre_initialized || !g_gre_tunnel.active)
        return -1;
    if (!outer_pkt || !inner_buf)
        return -1;

    /* Must contain at least outer IP header + GRE header */
    int min_len = (int)(sizeof(struct ip_header) + sizeof(struct gre_header));
    if (outer_len < min_len)
        return -1;

    /* Verify outer IP protocol is GRE */
    const struct ip_header *outer = (const struct ip_header *)outer_pkt;
    if (outer->protocol != GRE_PROTOCOL)
        return -1;

    /* Skip outer IP header (IHL may vary) */
    int ihl = (outer->version_ihl & 0x0F) * 4;
    if (ihl < 20 || ihl > outer_len)
        return -1;

    /* Point to the GRE header */
    const struct gre_header *gre =
        (const struct gre_header *)(outer_pkt + ihl);

    /* Validate GRE header */
    uint16_t gre_flags = ntohs(gre->flags);

    /* Version must be 0 */
    if ((gre_flags & GRE_FLAG_VERSION) != GRE_VERSION_0)
        return -1;

    /* Calculate GRE header size based on optional fields.
     * This implementation supports C (checksum), K (key), S (sequence)
     * optional fields so that we can skip past any combination. */
    int gre_hdr_size = (int)sizeof(struct gre_header);

    if (gre_flags & GRE_FLAG_C) {
        /* Checksum (2 bytes) + Reserved (2 bytes) */
        gre_hdr_size += 4;
    }
    if (gre_flags & GRE_FLAG_K) {
        /* Key (4 bytes) */
        gre_hdr_size += 4;
    }
    if (gre_flags & GRE_FLAG_S) {
        /* Sequence number (4 bytes) */
        gre_hdr_size += 4;
    }

    if (ihl + gre_hdr_size > outer_len)
        return -1;

    /* Inner packet starts after the GRE header */
    const uint8_t *inner_start = outer_pkt + ihl + gre_hdr_size;
    int inner_len = outer_len - ihl - gre_hdr_size;

    if (inner_len < 0)
        return -1;

    if (inner_len > inner_max)
        inner_len = inner_max;

    /* Verify the protocol type is one we understand */
    uint16_t proto = ntohs(gre->protocol_type);
    if (proto != GRE_ETH_TYPE_IP)
        return -1;  /* only IPv4 inner protocol supported for now */

    memcpy(inner_buf, inner_start, inner_len);
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
EXPORT_SYMBOL(gre_encapsulate);
EXPORT_SYMBOL(gre_decapsulate);
#endif /* MODULE */
