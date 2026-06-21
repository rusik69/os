#ifndef NET_IGMP_H
#define NET_IGMP_H

#include "types.h"
#include "net.h"

/* IGMP protocol constants */
#define IGMP_TYPE_MEMBERSHIP_QUERY   0x11
#define IGMP_TYPE_V1_MEMBERSHIP_REPORT 0x12
#define IGMP_TYPE_V2_MEMBERSHIP_REPORT 0x16
#define IGMP_TYPE_V2_LEAVE_GROUP     0x17
#define IGMP_TYPE_V3_MEMBERSHIP_REPORT 0x22

/* IP multicast group request (matching Linux ip_mreqn) */
struct ip_mreqn {
    uint32_t imr_multiaddr;   /* multicast group IP address */
    uint32_t imr_address;     /* local interface IP (0 = default) */
    int      imr_ifindex;     /* interface index (0 = default) */
};

/* Maximum tracked multicast groups */
#define IGMP_MAX_GROUPS 16

/* Tracked multicast group membership */
struct igmp_group {
    int      in_use;
    uint32_t multiaddr;   /* multicast group address */
    int      ifindex;     /* interface index */
    int      refcount;    /* number of joiners */
};

/* Join a multicast group.
 * mreq: pointer to an ip_mreqn describing the group.
 * Returns 0 on success, negative on error. */
int igmp_join_group(const struct ip_mreqn *mreq);

/* Leave a multicast group.
 * mreq: pointer to an ip_mreqn describing the group.
 * Returns 0 on success, negative on error. */
int igmp_leave_group(const struct ip_mreqn *mreq);

/* Process an incoming IGMP packet.
 * ip_hdr: the IP header of the received packet (payload follows header).
 * len: total length of the IP packet (for IGMPv3 parsing).
 * Called from the network stack when IP protocol == 2 (IGMP). */
void igmp_handle_report(struct ip_header *ip_hdr, uint16_t len);

/* Get the list of currently joined groups.
 * Returns pointer to the static table; *count is set to number of active entries. */
struct igmp_group *igmp_get_groups(int *count);

/* Initialise the IGMP subsystem. */
void igmp_init(void);

#endif /* NET_IGMP_H */
