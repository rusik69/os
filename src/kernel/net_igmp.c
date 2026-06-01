#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "kernel.h"
#include "net.h"
#include "net_internal.h"
#include "net_igmp.h"
#include "errno.h"

/* IGMP header (RFC 2236) */
struct igmp_header {
    uint8_t  type;
    uint8_t  max_resp_time;  /* for queries (in tenths of seconds) */
    uint16_t checksum;
    uint32_t group_addr;     /* multicast group address (in network byte order) */
} __attribute__((packed));

/* Static group membership table */
static struct igmp_group igmp_groups[IGMP_MAX_GROUPS];
static int igmp_initialised = 0;

void igmp_init(void)
{
    if (igmp_initialised)
        return;

    memset(igmp_groups, 0, sizeof(igmp_groups));
    igmp_initialised = 1;
    kprintf("[OK] igmp: IGMP multicast group management initialised (%d groups max)\n",
            IGMP_MAX_GROUPS);
}

/* Find a group slot */
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

static int group_find_free(void)
{
    for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
        if (!igmp_groups[i].in_use)
            return i;
    }
    return -ENOSPC;
}

/* Send an IGMP membership report for the given group.
 * type: IGMP_TYPE_V2_MEMBERSHIP_REPORT or IGMP_TYPE_V2_LEAVE_GROUP */
static int igmp_send_report(uint32_t multiaddr, uint32_t src_ip, int ifindex, uint8_t type)
{
    (void)src_ip;
    (void)ifindex;

    /* Build IGMP header */
    struct igmp_header igmp;
    memset(&igmp, 0, sizeof(igmp));
    igmp.type = type;
    igmp.max_resp_time = 0;
    igmp.group_addr = multiaddr;
    igmp.checksum = 0;
    igmp.checksum = net_checksum(&igmp, sizeof(igmp));

    /* Send as IP packet with protocol 2 (IGMP), TTL=1 */
    /* We craft the IP header directly via send_ip */
    /* send_ip() in the existing stack takes dst_ip, protocol, payload, len */
    send_ip(multiaddr, 2, &igmp, sizeof(igmp));

    kprintf("igmp: sent %s for group %d.%d.%d.%d on ifindex %d\n",
            type == IGMP_TYPE_V2_MEMBERSHIP_REPORT ? "report" : "leave",
            (multiaddr >> 24) & 0xFF, (multiaddr >> 16) & 0xFF,
            (multiaddr >> 8) & 0xFF, multiaddr & 0xFF,
            ifindex);
    return 0;
}

int igmp_join_group(const struct ip_mreqn *mreq)
{
    if (!mreq)
        return -EINVAL;
    if (!igmp_initialised)
        return -ENOSYS;

    uint32_t multiaddr = mreq->imr_multiaddr;
    int ifindex = mreq->imr_ifindex;

    /* Validate multicast address (224.0.0.0/4) */
    if ((multiaddr & 0xF0000000) != 0xE0000000)
        return -EINVAL;

    int idx = group_find_by_multiaddr(multiaddr, ifindex);
    if (idx >= 0) {
        /* Already joined — increment refcount */
        igmp_groups[idx].refcount++;
        return 0;
    }

    idx = group_find_free();
    if (idx < 0)
        return -ENOSPC;

    struct igmp_group *g = &igmp_groups[idx];
    g->in_use = 1;
    g->multiaddr = multiaddr;
    g->ifindex = ifindex;
    g->refcount = 1;

    /* Send initial membership report */
    igmp_send_report(multiaddr, mreq->imr_address, ifindex,
                     IGMP_TYPE_V2_MEMBERSHIP_REPORT);

    return 0;
}

int igmp_leave_group(const struct ip_mreqn *mreq)
{
    if (!mreq)
        return -EINVAL;
    if (!igmp_initialised)
        return -ENOSYS;

    uint32_t multiaddr = mreq->imr_multiaddr;
    int ifindex = mreq->imr_ifindex;

    int idx = group_find_by_multiaddr(multiaddr, ifindex);
    if (idx < 0)
        return -ENOENT;

    struct igmp_group *g = &igmp_groups[idx];
    g->refcount--;
    if (g->refcount <= 0) {
        /* Send leave group message */
        igmp_send_report(multiaddr, 0, ifindex, IGMP_TYPE_V2_LEAVE_GROUP);
        memset(g, 0, sizeof(*g));
    }

    return 0;
}

void igmp_handle_report(struct ip_header *ip_hdr)
{
    if (!ip_hdr || !igmp_initialised)
        return;

    /* IGMP payload follows the IP header */
    struct igmp_header *igmp = (struct igmp_header *)((uint8_t *)ip_hdr + 20);
    /* For robustness, we could check the IHL field, but assume default 20 */

    uint32_t src_ip = ip_hdr->src_ip;
    uint32_t group_ip = igmp->group_addr;

    switch (igmp->type) {
    case IGMP_TYPE_MEMBERSHIP_QUERY:
        /* General or group-specific query from a multicast router */
        kprintf("igmp: membership query from %d.%d.%d.%d for group %d.%d.%d.%d\n",
                (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                (group_ip >> 24) & 0xFF, (group_ip >> 16) & 0xFF,
                (group_ip >> 8) & 0xFF, group_ip & 0xFF);
        /* Reply with reports for our groups */
        for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
            if (!igmp_groups[i].in_use)
                continue;
            /* If group-specific query, only respond for that group */
            if (group_ip != 0 && igmp_groups[i].multiaddr != group_ip)
                continue;
            igmp_send_report(igmp_groups[i].multiaddr, net_our_ip,
                             igmp_groups[i].ifindex,
                             IGMP_TYPE_V2_MEMBERSHIP_REPORT);
        }
        break;

    case IGMP_TYPE_V1_MEMBERSHIP_REPORT:
    case IGMP_TYPE_V2_MEMBERSHIP_REPORT:
        /* Another host joined the same group — we don't need to do anything
         * specifically since IGMP is non-atomic, but we can note it. */
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
            if (igmp_groups[i].in_use)
                c++;
        }
        *count = c;
    }
    return igmp_groups;
}
