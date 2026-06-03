/* vxlan.c — VXLAN Tunnel Encapsulation (RFC 7348)
 *
 * Implements VXLAN tunnel encapsulation and decapsulation as a reusable
 * kernel networking component.  VXLAN wraps L2 Ethernet frames inside
 * UDP datagrams using IANA-assigned port 8472.
 *
 * Reference: RFC 7348 — Virtual eXtensible Local Area Network (VXLAN)
 *
 * Packet structure:
 *   [Outer Ethernet] [Outer IPv4] [Outer UDP] [VXLAN] [Inner Ethernet]
 *
 * The VXLAN header carries a 24-bit VNI that identifies the overlay
 * network segment.
 */

#define KERNEL_INTERNAL
#include "vxlan.h"
#include "net.h"        /* struct ip_header, struct udp_header, htons etc. */
#include "printf.h"
#include "string.h"
#include "types.h"

/* ── Static state ──────────────────────────────────────────────────── */

/* Table of active VXLAN tunnels, indexed by VNI hash */
static struct vxlan_tunnel g_vxlan_tunnels[VXLAN_MAX_TUNNELS];
static int vxlan_initialized = 0;

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Compute a UDP source port from the VNI for entropy on the outer
 * encapsulation.  This improves ECMP (Equal-Cost Multi-Path) hashing
 * in the underlay network. */
static uint16_t vxlan_src_port(uint32_t vni)
{
    /* Simple hash of VNI bits — rotate and XOR for better distribution */
    uint32_t h = vni;
    h ^= (h >> 16) ^ (h >> 8);
    /* Map into the ephemeral port range (49152–65535) */
    return (uint16_t)(49152 + (h % 16383));
}

/* Find a tunnel by VNI */
static struct vxlan_tunnel *find_tunnel_by_vni(uint32_t vni)
{
    for (int i = 0; i < VXLAN_MAX_TUNNELS; i++) {
        if (g_vxlan_tunnels[i].active && g_vxlan_tunnels[i].vni == vni)
            return &g_vxlan_tunnels[i];
    }
    return NULL;
}

/* Find a free tunnel slot */
static struct vxlan_tunnel *find_free_slot(void)
{
    for (int i = 0; i < VXLAN_MAX_TUNNELS; i++) {
        if (!g_vxlan_tunnels[i].active)
            return &g_vxlan_tunnels[i];
    }
    return NULL;
}

/* ── Initialization / Teardown ─────────────────────────────────────── */

int vxlan_init(void)
{
    if (vxlan_initialized)
        return 0;

    memset(g_vxlan_tunnels, 0, sizeof(g_vxlan_tunnels));
    vxlan_initialized = 1;
    kprintf("[OK] VXLAN tunnel initialized (UDP port %d, max %d tunnels)\n",
            VXLAN_UDP_PORT, VXLAN_MAX_TUNNELS);
    return 0;
}

void vxlan_exit(void)
{
    if (!vxlan_initialized)
        return;

    /* Tear down all active tunnels */
    for (int i = 0; i < VXLAN_MAX_TUNNELS; i++) {
        if (g_vxlan_tunnels[i].active) {
            kprintf("[VXLAN] Tunnel VNI=%u torn down on exit\n",
                    (unsigned int)g_vxlan_tunnels[i].vni);
            g_vxlan_tunnels[i].active = 0;
        }
    }

    vxlan_initialized = 0;
    kprintf("[OK] VXLAN tunnel subsystem shut down\n");
}

/* ── Tunnel lifecycle ──────────────────────────────────────────────── */

int vxlan_create_tunnel(uint32_t remote, uint32_t local, uint32_t vni)
{
    if (!vxlan_initialized)
        return -1;

    /* VNI is 24 bits; reject values >= 2^24 */
    if (vni > 0x00FFFFFF)
        return -1;

    /* Check for duplicate VNI */
    if (find_tunnel_by_vni(vni))
        return -1;  /* tunnel with this VNI already exists */

    /* Find a free slot */
    struct vxlan_tunnel *t = find_free_slot();
    if (!t)
        return -1;  /* no free tunnel slots */

    t->remote_ip = remote;
    t->local_ip  = local;
    t->vni       = vni;
    t->src_port  = vxlan_src_port(vni);
    t->active    = 1;

    kprintf("[VXLAN] Tunnel created: VNI=%u local %d.%d.%d.%d -> remote "
            "%d.%d.%d.%d (src_port=%u)\n",
            (unsigned int)vni,
            (uint8_t)(local  >> 24), (uint8_t)(local  >> 16),
            (uint8_t)(local  >> 8),  (uint8_t)(local),
            (uint8_t)(remote >> 24), (uint8_t)(remote >> 16),
            (uint8_t)(remote >> 8),  (uint8_t)(remote),
            (unsigned int)t->src_port);
    return 0;
}

int vxlan_destroy_tunnel(uint32_t vni)
{
    if (!vxlan_initialized)
        return -1;

    struct vxlan_tunnel *t = find_tunnel_by_vni(vni);
    if (!t)
        return -1;

    kprintf("[VXLAN] Tunnel destroyed: VNI=%u\n", (unsigned int)vni);
    memset(t, 0, sizeof(*t));
    return 0;
}

struct vxlan_tunnel *vxlan_tunnel_lookup(uint32_t vni)
{
    return find_tunnel_by_vni(vni);
}

/* ── Encapsulation ───────────────────────────────────────────────────
 *
 * Builds the outer IP + UDP + VXLAN headers around the inner Ethernet
 * frame.  The output layout is:
 *
 *   [IPv4 header (20)] [UDP header (8)] [VXLAN header (8)] [inner_eth]
 *
 * For production use, the outer IP header would need fragment-offset,
 * TTL, and checksum computation.  This implementation sets standard
 * values suitable for typical Ethernet/underlay MTUs.
 */

int vxlan_encapsulate(const uint8_t *inner_eth, int inner_len,
                      uint8_t *outer_buf, int outer_max, uint32_t vni)
{
    if (!vxlan_initialized)
        return -1;

    if (!inner_eth || inner_len <= 0 || !outer_buf || outer_max <= 0)
        return -1;

    /* Validate VNI (24-bit only) */
    if (vni > 0x00FFFFFF)
        return -1;

    /* Find the tunnel for this VNI to get endpoint addresses */
    struct vxlan_tunnel *t = find_tunnel_by_vni(vni);
    if (!t)
        return -1;

    /* Calculate total outer packet size:
     *   IPv4 header (20) + UDP header (8) + VXLAN header (8) + inner */
    const int outer_ip_hdr_len  = sizeof(struct ip_header);   /* 20 bytes no opts */
    const int udp_hdr_len       = sizeof(struct udp_header);  /* 8 bytes */
    const int vxlan_hdr_len     = (int)sizeof(struct vxlan_header); /* 8 bytes */
    const int total_overhead    = outer_ip_hdr_len + udp_hdr_len + vxlan_hdr_len;

    /* Ensure inner frame is at least minimum Ethernet payload size
     * (60 bytes without FCS, though we use 64 for safety) and pad
     * with zeros if needed. */
    int effective_inner_len = inner_len;
    const int min_eth_payload = 64;
    uint8_t pad_buf[64];
    if (inner_len < min_eth_payload) {
        memset(pad_buf, 0, sizeof(pad_buf));
        memcpy(pad_buf, inner_eth, (size_t)inner_len);
        inner_eth = pad_buf;
        effective_inner_len = min_eth_payload;
    }

    const int total_len = total_overhead + effective_inner_len;
    if (total_len > outer_max)
        return -1;

    /* ── Build outer IPv4 header ──────────────────────────────────── */
    struct ip_header *ip = (struct ip_header *)outer_buf;
    memset(ip, 0, sizeof(*ip));
    ip->version_ihl  = 0x45;                       /* IPv4 + IHL=5 (20 bytes) */
    ip->tos          = 0;
    ip->total_len    = htons((uint16_t)total_len);
    ip->id           = htons((uint16_t)(vni & 0xFFFF));  /* use VNI low bits */
    ip->flags_frag   = htons(0x4000);               /* Don't Fragment */
    ip->ttl          = 64;
    ip->protocol     = IP_PROTO_UDP;
    ip->checksum     = 0;
    ip->src_ip       = t->local_ip;
    ip->dst_ip       = t->remote_ip;

    /* Compute IP header checksum (ones-complement sum).
     * Use memcpy to access packed fields and avoid
     * -Waddress-of-packed-member warnings. */
    {
        uint32_t sum = 0;
        uint16_t tmp;
        const uint8_t *bytes = (const uint8_t *)ip;
        for (int i = 0; i < outer_ip_hdr_len / 2; i++) {
            memcpy(&tmp, bytes + i * 2, sizeof(tmp));
            sum += ntohs(tmp);
        }
        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);
        ip->checksum = htons((uint16_t)(~sum & 0xFFFF));
    }

    /* ── Build outer UDP header ───────────────────────────────────── */
    const int udp_payload_len = vxlan_hdr_len + effective_inner_len;
    struct udp_header *udp = (struct udp_header *)(outer_buf + outer_ip_hdr_len);
    udp->src_port = htons(t->src_port);
    udp->dst_port = htons(VXLAN_UDP_PORT);
    udp->length   = htons((uint16_t)(udp_hdr_len + udp_payload_len));
    udp->checksum = 0;  /* UDP checksum is optional for IPv4; set to 0 */

    /* ── Build VXLAN header ───────────────────────────────────────── */
    struct vxlan_header *vxlan = (struct vxlan_header *)
        (outer_buf + outer_ip_hdr_len + udp_hdr_len);

    /* Build VXLAN header: pack as two 32-bit big-endian words.
     * Use memcpy to avoid -Waddress-of-packed-member. */
    {
        uint32_t words[2];
        words[0] = htonl((uint32_t)VXLAN_FLAG_VNI << 24);
        words[1] = htonl((vni & 0x00FFFFFFU) << 8);
        memcpy(vxlan, words, sizeof(words));
    }

    /* ── Copy inner Ethernet frame ────────────────────────────────── */
    memcpy(outer_buf + total_overhead, inner_eth, (size_t)effective_inner_len);

    return total_len;
}

/* ── Decapsulation ───────────────────────────────────────────────────
 *
 * Parses the outer IP + UDP + VXLAN headers and extracts the inner
 * Ethernet frame.  Validates:
 *   1. IP protocol is UDP (17)
 *   2. UDP destination port is VXLAN port (8472)
 *   3. VXLAN header has the VNI flag set (0x08)
 *   4. VNI is 24-bit
 *   5. Output buffer fits the payload
 */

int vxlan_decapsulate(const uint8_t *outer_pkt, int outer_len,
                      uint8_t *inner_buf, int inner_max,
                      uint32_t *out_vni)
{
    if (!vxlan_initialized)
        return -1;

    if (!outer_pkt || outer_len <= 0 || !inner_buf || inner_max <= 0 || !out_vni)
        return -1;

    /* Minimum outer packet: IP hdr (20) + UDP (8) + VXLAN (8) = 36 bytes */
    const int outer_ip_hdr_min = (int)sizeof(struct ip_header);
    const int udp_hdr_len      = (int)sizeof(struct udp_header);
    const int vxlan_hdr_len    = (int)sizeof(struct vxlan_header);
    const int min_outer        = outer_ip_hdr_min + udp_hdr_len + vxlan_hdr_len;

    if (outer_len < min_outer)
        return -1;

    /* ── Parse outer IPv4 header ──────────────────────────────────── */
    const struct ip_header *ip = (const struct ip_header *)outer_pkt;

    /* Basic IP validation */
    if ((ip->version_ihl & 0xF0) != 0x40)  /* IPv4 */
        return -1;
    if (ip->protocol != IP_PROTO_UDP)
        return -1;

    /* IHL can be >5 if IP options present; advance to UDP header */
    int actual_ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
    if (actual_ip_hdr_len < outer_ip_hdr_min || actual_ip_hdr_len > outer_len)
        return -1;

    /* ── Parse outer UDP header ───────────────────────────────────── */
    const struct udp_header *udp = (const struct udp_header *)
        (outer_pkt + actual_ip_hdr_len);

    /* Validate UDP destination port is our VXLAN port */
    if (ntohs(udp->dst_port) != VXLAN_UDP_PORT)
        return -1;

    /* Validate UDP length */
    uint16_t udp_len = ntohs(udp->length);
    if (udp_len < (uint16_t)(udp_hdr_len + vxlan_hdr_len))
        return -1;  /* UDP payload too small for VXLAN header */

    /* ── Parse VXLAN header ───────────────────────────────────────── */
    const struct vxlan_header *vxlan = (const struct vxlan_header *)
        (outer_pkt + actual_ip_hdr_len + udp_hdr_len);

    /* Parse VXLAN header: read two 32-bit big-endian words.
     * Use memcpy to avoid -Waddress-of-packed-member. */
    uint32_t vxlan_words[2];
    memcpy(vxlan_words, vxlan, sizeof(vxlan_words));
    uint32_t word0 = ntohl(vxlan_words[0]);
    uint32_t word1 = ntohl(vxlan_words[1]);

    /* Check VNI flag */
    uint8_t flags = (uint8_t)(word0 >> 24);
    if (!(flags & VXLAN_FLAG_VNI))
        return -1;  /* VNI flag not set — invalid VXLAN frame */

    /* Extract VNI (24 bits from word1, right-shifted by 8) */
    uint32_t vni = (word1 >> 8) & 0x00FFFFFFU;
    *out_vni = vni;

    /* ── Extract inner Ethernet frame ─────────────────────────────── */
    int inner_offset = actual_ip_hdr_len + udp_hdr_len + vxlan_hdr_len;
    int inner_len = outer_len - inner_offset;

    if (inner_len <= 0)
        return -1;

    if (inner_len > inner_max)
        return -1;  /* output buffer too small */

    memcpy(inner_buf, outer_pkt + inner_offset, (size_t)inner_len);
    return inner_len;
}

/* ── Module support ────────────────────────────────────────────────── */
#ifdef MODULE
#include "module.h"

static int vxlan_module_init(void)
{
    return vxlan_init();
}

static void vxlan_module_exit(void)
{
    vxlan_exit();
}

MODULE_INIT(vxlan_module_init);
MODULE_EXIT(vxlan_module_exit);
MODULE_NAME("vxlan");
MODULE_DESCRIPTION("VXLAN Tunnel Encapsulation (RFC 7348)");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS");
MODULE_LICENSE("GPL");
#endif /* MODULE */
