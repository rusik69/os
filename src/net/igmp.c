/* igmp.c — IGMPv2/v3 multicast group management, join/leave, query/report */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "net.h"
#include "net_internal.h"
#include "net_igmp.h"
#include "errno.h"

/* IGMP header (RFC 2236, RFC 3376) */
struct igmp_header {
    uint8_t  type;
    uint8_t  max_resp_time;  /* for queries (in tenths of seconds) */
    uint16_t checksum;
    uint32_t group_addr;     /* multicast group address (network byte order) */
} __attribute__((packed));

/* IGMPv3 group record (RFC 3376 section 4.2.12) */
struct igmpv3_grec {
    uint8_t  record_type;    /* 1=MODE_IS_INCLUDE, 2=MODE_IS_EXCLUDE, etc. */
    uint8_t  aux_data_len;   /* length of auxiliary data (in 32-bit words) */
    uint16_t n_srcs;         /* number of source addresses */
    uint32_t group_addr;     /* multicast group address */
    /* Followed by n_srcs source addresses */
} __attribute__((packed));

/* IGMPv3 query (RFC 3376 section 4.1.2) */
struct igmpv3_query {
    uint8_t  type;           /* 0x11 */
    uint8_t  max_resp_code;  /* max response time encoded */
    uint16_t checksum;
    uint32_t group_addr;     /* 0 = general query, else group-specific */
    uint8_t  s_flags:4;      /* S flag (Suppress Router-side Processing) */
    uint8_t  qrv:3;          /* Querier's Robustness Variable */
    uint8_t  resv:1;         /* reserved */
    uint8_t  qqic;           /* Querier's Query Interval Code */
    uint16_t n_srcs;         /* number of source addresses */
    /* Followed by n_srcs source addresses */
} __attribute__((packed));

/* Group record types for IGMPv3 */
#define IGMPV3_MODE_IS_INCLUDE       1
#define IGMPV3_MODE_IS_EXCLUDE       2
#define IGMPV3_CHANGE_TO_INCLUDE     3
#define IGMPV3_CHANGE_TO_EXCLUDE     4
#define IGMPV3_ALLOW_NEW_SOURCES     5
#define IGMPV3_BLOCK_OLD_SOURCES     6

/* Static group membership table */
static struct igmp_group igmp_groups[IGMP_MAX_GROUPS];
static int igmp_initialised = 0;

/* Forward declarations from net.c / net_internal.h */
extern uint16_t net_checksum(const void *data, int len);

void igmp_init(void)
{
    if (igmp_initialised) return;
    memset(igmp_groups, 0, sizeof(igmp_groups));
    igmp_initialised = 1;
    kprintf("[OK] igmp: IGMPv2/v3 multicast initialised (%d groups max)\n",
            IGMP_MAX_GROUPS);
}

/* Find a group slot by multicast address and interface */
static int group_find_by_multiaddr(uint32_t multiaddr, int ifindex)
{
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (igmp_groups[i].in_use &&
            igmp_groups[i].multiaddr == multiaddr &&
            igmp_groups[i].ifindex == ifindex)
            return i;
    }
    return -ENOENT;
}

/* Find a free group slot */
static int group_find_free(void)
{
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (!igmp_groups[i].in_use) return i;
    }
    return -ENOSPC;
}

/* Send IGMP message to a multicast group */
static int igmp_send(uint32_t multiaddr, uint8_t type, uint32_t src_ip,
                     int ifindex, uint8_t max_resp_time)
{
    (void)src_ip;
    (void)ifindex;

    struct igmp_header igmp;
    memset(&igmp, 0, sizeof(igmp));
    igmp.type = type;
    igmp.max_resp_time = max_resp_time;
    igmp.group_addr = multiaddr;
    igmp.checksum = 0;
    igmp.checksum = net_checksum(&igmp, sizeof(igmp));

    /* Send as IP packet, protocol=IGMP(2), TTL=1 */
    send_ip(multiaddr, 2, &igmp, sizeof(igmp));

    kprintf("igmp: sent type=0x%02x for %d.%d.%d.%d on ifindex %d\n",
            type,
            (multiaddr >> 24) & 0xFF, (multiaddr >> 16) & 0xFF,
            (multiaddr >> 8) & 0xFF, multiaddr & 0xFF,
            ifindex);
    return 0;
}

int igmp_join_group(const struct ip_mreqn *mreq)
{
    if (!mreq) return -EINVAL;
    if (!igmp_initialised) return -ENOSYS;

    uint32_t multiaddr = mreq->imr_multiaddr;
    int ifindex = mreq->imr_ifindex;

    /* Validate multicast address (224.0.0.0/4) */
    if ((multiaddr & 0xF0000000) != 0xE0000000)
        return -EINVAL;

    int idx = group_find_by_multiaddr(multiaddr, ifindex);
    if (idx >= 0) {
        igmp_groups[idx].refcount++;
        return 0;
    }

    idx = group_find_free();
    if (idx < 0) return -ENOSPC;

    struct igmp_group *g = &igmp_groups[idx];
    g->in_use = 1;
    g->multiaddr = multiaddr;
    g->ifindex = ifindex;
    g->refcount = 1;

    /* Send initial IGMPv2 membership report */
    igmp_send(multiaddr, IGMP_TYPE_V2_MEMBERSHIP_REPORT,
              mreq->imr_address, ifindex, 0);

    return 0;
}

int igmp_leave_group(const struct ip_mreqn *mreq)
{
    if (!mreq) return -EINVAL;
    if (!igmp_initialised) return -ENOSYS;

    uint32_t multiaddr = mreq->imr_multiaddr;
    int ifindex = mreq->imr_ifindex;

    int idx = group_find_by_multiaddr(multiaddr, ifindex);
    if (idx < 0) return -ENOENT;

    struct igmp_group *g = &igmp_groups[idx];
    g->refcount--;
    if (g->refcount <= 0) {
        igmp_send(multiaddr, IGMP_TYPE_V2_LEAVE_GROUP, 0, ifindex, 0);
        memset(g, 0, sizeof(*g));
    }

    return 0;
}

/* Process incoming IGMP membership query/report */
void igmp_handle_report(struct ip_header *ip_hdr)
{
    if (!ip_hdr || !igmp_initialised) return;

    /* IGMP payload follows the IP header (assume 20-byte IHL) */
    struct igmp_header *igmp = (struct igmp_header *)((uint8_t *)ip_hdr + 20);
    uint32_t src_ip = ip_hdr->src_ip;
    uint32_t group_ip = igmp->group_addr;

    switch (igmp->type) {
    case IGMP_TYPE_MEMBERSHIP_QUERY: {
        /* Determine if this is IGMPv3 query */
        struct igmpv3_query *q = (struct igmpv3_query *)igmp;
        int is_v3 = (ip_hdr->total_len > 24); /* IGMPv3 query has more fields */

        kprintf("igmp: membership query from %d.%d.%d.%d group=%d.%d.%d.%d%s\n",
                (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                (group_ip >> 24) & 0xFF, (group_ip >> 16) & 0xFF,
                (group_ip >> 8) & 0xFF, group_ip & 0xFF,
                is_v3 ? " (IGMPv3)" : "");

        /* Respond with reports for our groups */
        for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
            if (!igmp_groups[i].in_use) continue;
            if (group_ip != 0 && igmp_groups[i].multiaddr != group_ip)
                continue;
            igmp_send(igmp_groups[i].multiaddr, IGMP_TYPE_V2_MEMBERSHIP_REPORT,
                      net_our_ip, igmp_groups[i].ifindex,
                      is_v3 ? q->max_resp_code : igmp->max_resp_time);
        }
        break;
    }

    case IGMP_TYPE_V1_MEMBERSHIP_REPORT:
    case IGMP_TYPE_V2_MEMBERSHIP_REPORT:
        kprintf("igmp: report from %d.%d.%d.%d for group %d.%d.%d.%d\n",
                (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                (group_ip >> 24) & 0xFF, (group_ip >> 16) & 0xFF,
                (group_ip >> 8) & 0xFF, group_ip & 0xFF);
        break;

    case IGMP_TYPE_V2_LEAVE_GROUP:
        kprintf("igmp: leave from %d.%d.%d.%d for group %d.%d.%d.%d\n",
                (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                (group_ip >> 24) & 0xFF, (group_ip >> 16) & 0xFF,
                (group_ip >> 8) & 0xFF, group_ip & 0xFF);
        break;

    case IGMP_TYPE_V3_MEMBERSHIP_REPORT:
        kprintf("igmp: v3 report from %d.%d.%d.%d\n",
                (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                (src_ip >> 8) & 0xFF, src_ip & 0xFF);
        break;

    default:
        kprintf("igmp: unknown type 0x%02x from %d.%d.%d.%d\n",
                igmp->type,
                (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                (src_ip >> 8) & 0xFF, src_ip & 0xFF);
        break;
    }
}

struct igmp_group *igmp_get_groups(int *count)
{
    if (!igmp_initialised) {
        if (count) *count = 0;
        return NULL;
    }

    if (count) {
        int c = 0;
        for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
            if (igmp_groups[i].in_use) c++;
        }
        *count = c;
    }
    return igmp_groups;
}
