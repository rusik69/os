/* ipip.c — IP-in-IP tunneling */

#define KERNEL_INTERNAL
#include "ipip.h"
#include "net.h"
#include "printf.h"
#include "string.h"

static struct ipip_tunnel g_tunnel;
static int ipip_initialized = 0;

int ipip_init(void) {
    memset(&g_tunnel, 0, sizeof(g_tunnel));
    ipip_initialized = 1;
    kprintf("[OK] IPIP tunnel initialized\\n");
    return 0;
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
