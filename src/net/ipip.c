/* ipip.c — IP-in-IP tunneling (loadable kernel module) */

#define KERNEL_INTERNAL
#include "ipip.h"
#include "net.h"
#include "printf.h"
#include "string.h"

#ifdef MODULE
#include "export.h"
#include "module.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("IP-in-IP tunnel protocol (RFC 2003) - loadable kernel module");
MODULE_ALIAS("net-ipip");
#endif

static struct ipip_tunnel g_tunnel;
static int ipip_initialized = 0;

int ipip_init(void) {
    if (ipip_initialized)
        return 0;
    memset(&g_tunnel, 0, sizeof(g_tunnel));
    ipip_initialized = 1;
    kprintf("[OK] IPIP tunnel initialized\n");
    return 0;
}

void ipip_exit(void) {
    if (!ipip_initialized)
        return;
    /* Tear down any active tunnel */
    g_tunnel.active = 0;
    ipip_initialized = 0;
    kprintf("[OK] IPIP tunnel shut down\n");
}

int ipip_create_tunnel(uint32_t remote, uint32_t local) {
    if (!ipip_initialized) return -1;
    if (g_tunnel.active) return -1;

    g_tunnel.remote_ip = remote;
    g_tunnel.local_ip  = local;
    g_tunnel.active    = 1;
    return 0;
}

int ipip_destroy_tunnel(void) {
    if (!ipip_initialized || !g_tunnel.active) return -1;
    g_tunnel.active = 0;
    return 0;
}

int ipip_encapsulate(const uint8_t *inner_pkt, int inner_len,
                      uint8_t *outer_buf, int outer_max) {
    if (!ipip_initialized || !g_tunnel.active) return -1;
    if (!inner_pkt || !outer_buf) return -1;
    if (inner_len < 20) return -1;  /* need at least IP header */
    if (inner_len + (int)sizeof(struct ip_header) > outer_max) return -1;

    struct ip_header *outer = (struct ip_header *)outer_buf;
    memset(outer, 0, sizeof(struct ip_header));
    outer->version_ihl = 0x45;
    outer->ttl = 64;
    outer->protocol = IPIP_PROTOCOL;  /* IP-in-IP */
    outer->src_ip = htonl(g_tunnel.local_ip);
    outer->dst_ip = htonl(g_tunnel.remote_ip);
    outer->total_len = htons((uint16_t)(sizeof(struct ip_header) + inner_len));
    outer->id = htons(1);
    outer->checksum = 0;
    outer->checksum = net_checksum(outer, sizeof(struct ip_header));

    memcpy(outer_buf + sizeof(struct ip_header), inner_pkt, inner_len);
    return sizeof(struct ip_header) + inner_len;
}

int ipip_decapsulate(const uint8_t *outer_pkt, int outer_len,
                      uint8_t *inner_buf, int inner_max) {
    if (!ipip_initialized || !g_tunnel.active) return -1;
    if (!outer_pkt || !inner_buf) return -1;
    if (outer_len < (int)sizeof(struct ip_header) + 20) return -1;

    struct ip_header *outer = (struct ip_header *)outer_pkt;
    if (outer->protocol != IPIP_PROTOCOL) return -1;

    int ihl = (outer->version_ihl & 0xF) * 4;
    int inner_len = outer_len - ihl;

    if (inner_len > inner_max) inner_len = inner_max;
    memcpy(inner_buf, outer_pkt + ihl, inner_len);
    return inner_len;
}

#ifdef MODULE
/* Module entry points — the ELF module loader looks for these symbols */
int init_module(void) {
    return ipip_init();
}

void cleanup_module(void) {
    ipip_exit();
}

/* Export symbols for other modules that may depend on IPIP tunneling */
EXPORT_SYMBOL(ipip_create_tunnel);
EXPORT_SYMBOL(ipip_destroy_tunnel);
EXPORT_SYMBOL(ipip_encapsulate);
EXPORT_SYMBOL(ipip_decapsulate);
#endif

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Implement: ipip_xmit ────────────────── */
int ipip_xmit(void *skb, void *dev)
{
    if (!skb || !dev) {
        kprintf("[ipip] ipip_xmit: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[ipip] ipip_xmit: skb=%p dev=%p (stub)\n", skb, dev);
    return -EOPNOTSUPP;
}
/* ── Implement: ipip_rcv ────────────────── */
int ipip_rcv(void *skb)
{
    if (!skb) {
        kprintf("[ipip] ipip_rcv: NULL skb\n");
        return -EINVAL;
    }
    kprintf("[ipip] ipip_rcv: skb=%p (stub)\n", skb);
    return -EOPNOTSUPP;
}
/* ── Stub: ipip_err ────────────────────────────────── */
void ipip_err(void *skb, uint32_t info)
{
    (void)skb;
    (void)info;
    kprintf("[IPIP] ipip_err: not yet implemented\n");
}

