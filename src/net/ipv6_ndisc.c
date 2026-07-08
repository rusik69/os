/*
 * ipv6_ndisc.c — IPv6 Neighbor Discovery Protocol (NDP)
 *
 * Implements RFC 4861 Neighbor Solicitation (NS) / Neighbor Advertisement (NA)
 * with a full neighbor cache including reachability state tracking.
 *
 * Public interface:
 *   ipv6_nd_init()       — initialise neighbor cache
 *   ipv6_nd_poll()       — periodic reachability checks
 *   ipv6_nd_send_ns()    — send Neighbor Solicitation for a target
 *   ipv6_nd_send_na()    — send Neighbor Advertisement
 *   ipv6_nd_handle_ns()  — process incoming NS
 *   ipv6_nd_handle_na()  — process incoming NA
 *   ipv6_nd_cache_add()  — add / update neighbor cache entry
 *   ipv6_nd_cache_lookup() — look up MAC for an IPv6 address
 *   ipv6_calc_solicited_node() — compute solicited-node multicast address
 */

#define KERNEL_INTERNAL
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define ND_CACHE_SIZE     8      /* max entries in neighbor cache */
#define ND_REACHABLE_TIME 3000  /* reachable time in ticks (30 s at 100 Hz) */
#define ND_RETRANS_TIMER  100   /* retransmit timer in ticks (1 s at 100 Hz) */
#define ND_MAX_UNICAST    3     /* max unicast NS probes before multicast */

/* ── Reachability state (RFC 4861 §7.3.2) ───────────────────────────
 *
 * INCOMPLETE  — NS sent, no NA received yet (address resolution in progress)
 * REACHABLE   — positive confirmation within ReachableTime
 * STALE       — ReachableTime expired, no new confirmation
 * DELAY       — packet sent while STALE, waiting DelayFirstProbeTime
 * PROBE       — unicast NS sent, waiting for NA
 */

enum nd_reach_state {
	ND_REACH_INCOMPLETE = 0,
	ND_REACH_REACHABLE  = 1,
	ND_REACH_STALE      = 2,
	ND_REACH_DELAY      = 3,
	ND_REACH_PROBE      = 4,
};

struct nd_cache_entry {
	struct in6_addr ip6;             /* neighbor IPv6 address */
	uint8_t  mac[6];                 /* resolved MAC address */
	uint8_t  is_router;              /* 1 = neighbor is a router */
	int      valid;                  /* 1 = slot in use */
	int      state;                  /* ND_REACH_* state */
	uint64_t last_confirmed;         /* tick when reachability confirmed */
	uint64_t last_probe;             /* tick when last NS probe sent */
	int      probe_count;            /* consecutive probes sent */
};

static struct nd_cache_entry nd_cache[ND_CACHE_SIZE];

/* ── Forward declarations ───────────────────────────────────────── */

static void nd_send_na_internal(const struct in6_addr *target,
                                const struct in6_addr *dst,
                                int solicited, int override,
                                const struct in6_addr *src_override);

/* ── RA state management ────────────────────────────────────────────
 *
 * Tracks default routers and prefix information received via RA.
 * Used for SLAAC, prefix lifetime expiry, and route selection.
 */

/* Default router list */
#define ND_ROUTER_LIST_SIZE 4

struct nd_router_entry {
	struct in6_addr addr;            /* router IPv6 address */
	uint16_t lifetime;               /* router lifetime in seconds (0 = not a default router) */
	uint64_t expiry_tick;            /* tick when entry expires */
	int      valid;                  /* 1 = slot in use */
};

static struct nd_router_entry nd_routers[ND_ROUTER_LIST_SIZE];
static int nd_router_count = 0;

/* Prefix list — tracks prefixes received via RA */
#define ND_PREFIX_LIST_SIZE 8

struct nd_prefix_entry {
	struct in6_addr prefix;
	uint8_t  prefix_len;
	uint32_t valid_lifetime;         /* seconds */
	uint32_t preferred_lifetime;
	uint64_t expiry_tick;            /* tick when valid_lifetime expires */
	uint8_t  flags;                  /* L=bit7, A=bit6 */
	int      valid;
};

static struct nd_prefix_entry nd_prefixes[ND_PREFIX_LIST_SIZE];
static int nd_prefix_count = 0;

/* Forward declarations for RA prefix/router expiry */
static void nd_expire_prefixes(void);
static void nd_expire_routers(void);

/* ── Solicited-node multicast ───────────────────────────────────── */

void ipv6_calc_solicited_node(const struct in6_addr *addr,
                               struct in6_addr *mcast)
{
	mcast->s6_addr[0]  = 0xFF;
	mcast->s6_addr[1]  = 0x02;
	mcast->s6_addr[2]  = 0x00;
	mcast->s6_addr[3]  = 0x00;
	mcast->s6_addr[4]  = 0x00;
	mcast->s6_addr[5]  = 0x00;
	mcast->s6_addr[6]  = 0x00;
	mcast->s6_addr[7]  = 0x00;
	mcast->s6_addr[8]  = 0x00;
	mcast->s6_addr[9]  = 0x00;
	mcast->s6_addr[10] = 0x00;
	mcast->s6_addr[11] = 0x01;
	mcast->s6_addr[12] = 0xFF;
	/* Take last 3 bytes of address */
	mcast->s6_addr[13] = addr->s6_addr[13];
	mcast->s6_addr[14] = addr->s6_addr[14];
	mcast->s6_addr[15] = addr->s6_addr[15];
}

/* ── Neighbour Cache Management ─────────────────────────────────── */

void ipv6_nd_cache_add(const struct in6_addr *ip6, const uint8_t *mac)
{
	struct nd_cache_entry *entry = NULL;
	int i;

	/* Check for existing entry — update it */
	for (i = 0; i < ND_CACHE_SIZE; i++) {
		if (nd_cache[i].valid &&
		    ipv6_addr_equal(&nd_cache[i].ip6, ip6)) {
			entry = &nd_cache[i];
			break;
		}
	}

	if (entry) {
		/* Update MAC and state */
		memcpy(entry->mac, mac, 6);
		entry->last_confirmed = timer_get_ticks();
		entry->state = ND_REACH_REACHABLE;
		entry->probe_count = 0;
		entry->is_router = 0;  /* caller can override if NA has R flag */
		return;
	}

	/* Find free slot, or LRU-replace oldest REACHABLE/STALE entry */
	int slot = -1;
	uint64_t oldest = UINT64_MAX;

	for (i = 0; i < ND_CACHE_SIZE; i++) {
		if (!nd_cache[i].valid) {
			slot = i;
			break;
		}
		if (nd_cache[i].last_confirmed < oldest &&
		    nd_cache[i].state != ND_REACH_INCOMPLETE &&
		    nd_cache[i].state != ND_REACH_PROBE) {
			oldest = nd_cache[i].last_confirmed;
			slot = i;
		}
	}

	if (slot < 0) {
		/* All slots busy with incomplete/probe — drop oldest */
		for (i = 0; i < ND_CACHE_SIZE; i++) {
			if (nd_cache[i].last_confirmed < oldest) {
				oldest = nd_cache[i].last_confirmed;
				slot = i;
			}
		}
	}

	if (slot >= 0) {
		memset(&nd_cache[slot], 0, sizeof(nd_cache[slot]));
		memcpy(&nd_cache[slot].ip6, ip6, sizeof(struct in6_addr));
		memcpy(nd_cache[slot].mac, mac, 6);
		nd_cache[slot].valid = 1;
		nd_cache[slot].state = ND_REACH_REACHABLE;
		nd_cache[slot].last_confirmed = timer_get_ticks();
		nd_cache[slot].probe_count = 0;
	}
}

uint8_t *ipv6_nd_cache_lookup(const struct in6_addr *ip6)
{
	int i;

	for (i = 0; i < ND_CACHE_SIZE; i++) {
		if (nd_cache[i].valid &&
		    ipv6_addr_equal(&nd_cache[i].ip6, ip6))
			return nd_cache[i].mac;
	}
	return NULL;
}

/* ── Send Neighbor Solicitation (RFC 4861 §7.2.2) ───────────────────
 *
 * Sends an NS for 'target' to the solicited-node multicast address.
 * Includes Source Link-layer Address option if we have a link-local
 * address configured. */

int ipv6_nd_send_ns(const struct in6_addr *target)
{
	uint8_t buf[128];
	struct nd_neighbor *ns;
	struct nd_option *opt;
	struct in6_addr mcast;
	uint16_t ns_len;
	int i;

	if (!target)
		return -EINVAL;

	memset(buf, 0, sizeof(buf));
	ns = (struct nd_neighbor *)buf;
	ns->icmp.type = ICMPV6_NS;
	ns->icmp.code = 0;
	ns->icmp.checksum = 0;
	memset(&ns->reserved, 0, sizeof(ns->reserved));
	memcpy(&ns->target, target, sizeof(struct in6_addr));

	/* Source Link-layer Address option */
	opt = (struct nd_option *)(buf + sizeof(struct nd_neighbor));
	opt->type = ND_OPT_SRC_LLADDR;
	opt->len  = 1;  /* 1 × 8 = 8 bytes */
	memcpy(buf + sizeof(struct nd_neighbor) + 2, net_our_mac, 6);

	ns_len = sizeof(struct nd_neighbor) + 8;

	/* Compute checksum */
	ns->icmp.checksum = ipv6_checksum(&net_our_ipv6_ll, target,
	                                  IP_PROTO_ICMPV6, buf, ns_len);

	/* Send to solicited-node multicast */
	ipv6_calc_solicited_node(target, &mcast);
	send_ipv6(&mcast, IP_PROTO_ICMPV6, buf, ns_len);

	/* Update cache entry state to INCOMPLETE if we're tracking */
	for (i = 0; i < ND_CACHE_SIZE; i++) {
		if (nd_cache[i].valid &&
		    ipv6_addr_equal(&nd_cache[i].ip6, target)) {
			nd_cache[i].state = ND_REACH_INCOMPLETE;
			nd_cache[i].last_probe = timer_get_ticks();
			nd_cache[i].probe_count++;
			break;
		}
	}

	return 0;
}

/* ── Send Neighbor Advertisement (RFC 4861 §7.2.4) ──────────────────
 *
 * Sends an NA for 'target' to 'dst'.  If solicited is 1, the S flag
 * is set.  If override is 1, the O flag is set (override existing
 * cache entries).  If src_override is non-NULL, use that as the
 * source address; otherwise use our link-local. */

int ipv6_nd_send_na(const struct in6_addr *target,
                     const struct in6_addr *dst,
                     int solicited, int override,
                     const struct in6_addr *src_override)
{
	uint8_t buf[128];
	struct nd_neighbor *na;
	struct nd_option *opt;
	uint16_t nd_len;
	const struct in6_addr *src;

	if (!target || !dst)
		return -EINVAL;

	memset(buf, 0, sizeof(buf));
	na = (struct nd_neighbor *)buf;
	na->icmp.type = ICMPV6_NA;
	na->icmp.code = 0;

	/* Flags: R=0 (we are not a router), S=solicited, O=override */
	na->reserved = htonl((solicited ? 0x40000000U : 0) |
	                      (override ? 0x20000000U : 0));
	memcpy(&na->target, target, sizeof(struct in6_addr));

	/* Target Link-layer Address option */
	opt = (struct nd_option *)(buf + sizeof(struct nd_neighbor));
	opt->type = ND_OPT_TGT_LLADDR;
	opt->len  = 1;
	memcpy(buf + sizeof(struct nd_neighbor) + 2, net_our_mac, 6);

	nd_len = sizeof(struct nd_neighbor) + 8;

	/* Select source address */
	if (src_override)
		src = src_override;
	else
		src = &net_our_ipv6_ll;

	na->icmp.checksum = ipv6_checksum(src, dst,
	                                   IP_PROTO_ICMPV6, buf, nd_len);
	send_ipv6(dst, IP_PROTO_ICMPV6, buf, nd_len);
	return 0;
}

/* ── Handle incoming Neighbor Solicitation (RFC 4861 §7.2.3) ────────
 *
 * If the target address is one of ours, respond with a Neighbor
 * Advertisement.  Extract the source MAC from the Source Link-layer
 * Address option and update the neighbor cache. */

void ipv6_nd_handle_ns(struct ipv6_header *ip6,
                        const uint8_t *payload, uint16_t len)
{
	const struct nd_neighbor *ns;
	const struct nd_option *opt;
	int opt_offset;
	int is_our_ll;
	int is_our_gua;

	if (!ip6 || !payload || len < sizeof(struct nd_neighbor))
		return;

	ns = (const struct nd_neighbor *)payload;

	/* ── DAD conflict detection ─────────────────────────────────────
	 *
	 * Per RFC 4862 §5.4.2:
	 *  - If an NS with source != :: is received for a TENTATIVE address,
	 *    the address is a duplicate (another node already owns it).
	 *  - A node MUST NOT respond to an NS for a tentative address.
	 */
	{
		struct ipv6_addr_entry *tgt_entry = ipv6_addr_find(&ns->target);
		if (tgt_entry && tgt_entry->state == IPV6_ADDR_STATE_TENTATIVE) {
			/* NS with non-unspecified source means another node owns it */
			if (!ipv6_addr_is_unspecified(&ip6->src_ip)) {
				ipv6_dad_conflict(&ns->target);
			}
			/* Per RFC 4862 §5.4.2: do NOT respond to NS for tentative addr */
			return;
		}
	}

	/* We only respond if the target is one of our addresses */
	if (!net_ipv6_ll_ready)
		return;

	is_our_ll = ipv6_addr_equal(&ns->target, &net_our_ipv6_ll);
	is_our_gua = net_ipv6_gua_valid &&
	             ipv6_addr_equal(&ns->target, &net_our_ipv6_gua);

	if (!is_our_ll && !is_our_gua)
		return;

	/* Extract source MAC from Source Link-layer Address option */
	opt = (const struct nd_option *)(payload + sizeof(struct nd_neighbor));
	opt_offset = sizeof(struct nd_neighbor);

	while (opt_offset + 2 <= (int)len) {
		if (opt->type == ND_OPT_SRC_LLADDR && opt->len == 1) {
			ipv6_nd_cache_add(&ip6->src_ip,
			                  payload + opt_offset + 2);
			break;
		}
		opt_offset += (int)opt->len * 8;
		if (opt_offset + 2 > (int)len)
			break;
		opt = (const struct nd_option *)(payload + opt_offset);
	}

	/* Send Neighbor Advertisement */
	if (ipv6_addr_is_unspecified(&ip6->src_ip)) {
		/* DAD — unsolicited NA to all-nodes multicast */
		struct in6_addr all_nodes = IPV6_ADDR_ALL_NODES;
		nd_send_na_internal(&ns->target, &all_nodes, 0, 1, NULL);
	} else {
		nd_send_na_internal(&ns->target, &ip6->src_ip, 1, 1, NULL);
	}
}

/* ── Handle incoming Neighbor Advertisement (RFC 4861 §7.2.5) ───────
 *
 * Extract the target MAC from the Target Link-layer Address option
 * and update the neighbor cache. */

void ipv6_nd_handle_na(struct ipv6_header *ip6,
                        const uint8_t *payload, uint16_t len)
{
	const struct nd_neighbor *na;
	const struct nd_option *opt;
	int opt_offset;

	(void)ip6;

	if (!payload || len < sizeof(struct nd_neighbor))
		return;

	na = (const struct nd_neighbor *)payload;

	/* ── DAD conflict detection ─────────────────────────────────────
	 * Per RFC 4862 §5.4.2: if an NA is received for a TENTATIVE address,
	 * the address is a duplicate.  Transition to DETACHED state.
	 */
	{
		struct ipv6_addr_entry *entry = ipv6_addr_find(&na->target);
		if (entry && entry->state == IPV6_ADDR_STATE_TENTATIVE) {
			ipv6_dad_conflict(&na->target);
			/* Still update cache with the source MAC for completeness */
		}
	}

	/* Extract target MAC from Target Link-layer Address option */
	opt = (const struct nd_option *)(payload + sizeof(struct nd_neighbor));
	opt_offset = sizeof(struct nd_neighbor);

	while (opt_offset + 2 <= (int)len) {
		if (opt->type == ND_OPT_TGT_LLADDR && opt->len == 1) {
			ipv6_nd_cache_add(&na->target,
			                  payload + opt_offset + 2);

			/* Mark as router if R flag is set */
			{
				int i;
				uint32_t flags = ntohl(na->reserved);
				int is_router = (flags & 0x80000000U) != 0;

				if (is_router) {
					for (i = 0; i < ND_CACHE_SIZE; i++) {
						if (nd_cache[i].valid &&
						    ipv6_addr_equal(&nd_cache[i].ip6,
						                     &na->target)) {
							nd_cache[i].is_router = 1;
							break;
						}
					}
				}
			}
			break;
		}
		opt_offset += (int)opt->len * 8;
		if (opt_offset + 2 > (int)len)
			break;
		opt = (const struct nd_option *)(payload + opt_offset);
	}
}

/* ── Internal NA sender (used from handle_ns, avoids re-entry) ───── */

static void nd_send_na_internal(const struct in6_addr *target,
                                 const struct in6_addr *dst,
                                 int solicited, int override,
                                 const struct in6_addr *src_override)
{
	ipv6_nd_send_na(target, dst, solicited, override, src_override);
}

/* ── Reachability state machine (RFC 4861 §7.3.2) ────────────────── */

void ipv6_nd_poll(void)
{
	uint64_t now = timer_get_ticks();
	int i;

	/* Expire stale prefixes and router entries from RA */
	nd_expire_prefixes();
	nd_expire_routers();

	for (i = 0; i < ND_CACHE_SIZE; i++) {
		struct nd_cache_entry *e = &nd_cache[i];

		if (!e->valid)
			continue;

		switch (e->state) {
		case ND_REACH_REACHABLE:
			/* Reachable time expired → STALE */
			if (now - e->last_confirmed >= ND_REACHABLE_TIME) {
				e->state = ND_REACH_STALE;
			}
			break;

		case ND_REACH_STALE:
			/* STALE entries are aged naturally.
			 * They become DELAY when a packet is sent to them
			 * (handled externally by ipv6_resolve_dst_mac).
			 * We don't auto-transition from STALE. */
			break;

		case ND_REACH_DELAY:
			/* Delay expired → PROBE */
			if (now - e->last_confirmed >= ND_REACHABLE_TIME) {
				e->state = ND_REACH_PROBE;
				e->probe_count = 0;
				e->last_probe = now;
				/* Send unicast NS probe */
				ipv6_nd_send_ns(&e->ip6);
			}
			break;

		case ND_REACH_PROBE:
			/* Check retransmission */
			if (now - e->last_probe >= ND_RETRANS_TIMER) {
				if (e->probe_count < ND_MAX_UNICAST) {
					e->probe_count++;
					e->last_probe = now;
					ipv6_nd_send_ns(&e->ip6);
				} else {
					/* Max probes reached — mark invalid */
					kprintf("[ndisc] neighbour %p: "
					        "resolution failed\n",
					        (void *)&e->ip6);
					memset(e, 0, sizeof(*e));
				}
			}
			break;

		case ND_REACH_INCOMPLETE:
			/* Retransmit NS if no response */
			if (now - e->last_probe >= ND_RETRANS_TIMER) {
				if (e->probe_count < ND_MAX_UNICAST) {
					e->probe_count++;
					e->last_probe = now;
					ipv6_nd_send_ns(&e->ip6);
				} else {
					kprintf("[ndisc] neighbour %p: "
					        "INCOMPLETE timeout\n",
					        (void *)&e->ip6);
					memset(e, 0, sizeof(*e));
				}
			}
			break;

		default:
			break;
		}
	}
}

/* ── Initialisation ──────────────────────────────────────────────── */

void ipv6_nd_init(void)
{
	memset(nd_cache, 0, sizeof(nd_cache));
	memset(nd_routers, 0, sizeof(nd_routers));
	nd_router_count = 0;
	memset(nd_prefixes, 0, sizeof(nd_prefixes));
	nd_prefix_count = 0;
	net_ipv6_gua_valid = 0;
	kprintf("[ndisc] IPv6 Neighbor Discovery initialised\n");
}

/* ── Router Solicitation (RFC 4861 §6.1.1) ──────────────────────────
 *
 * Sends a Router Solicitation to the all-routers multicast address
 * (FF02::2) with our source link-layer address.
 * Returns 0 on success, negative errno on failure. */

int ipv6_nd_send_rs(void)
{
	uint8_t buf[64];
	struct nd_router_solicit *rs;
	struct nd_option *opt;
	struct in6_addr all_routers = IPV6_ADDR_ALL_ROUTERS;
	uint16_t rs_len;

	if (!net_ipv6_ll_ready)
		return -ENETDOWN;

	memset(buf, 0, sizeof(buf));
	rs = (struct nd_router_solicit *)buf;
	rs->icmp.type = ICMPV6_RS;
	rs->icmp.code = 0;
	rs->icmp.checksum = 0;
	rs->reserved = 0;

	/* Source Link-layer Address option */
	opt = (struct nd_option *)(buf + sizeof(struct nd_router_solicit));
	opt->type = ND_OPT_SRC_LLADDR;
	opt->len  = 1;
	memcpy(buf + sizeof(struct nd_router_solicit) + 2, net_our_mac, 6);

	rs_len = sizeof(struct nd_router_solicit) + 8;

	/* Compute ICMPv6 checksum */
	rs->icmp.checksum = ipv6_checksum(&net_our_ipv6_ll, &all_routers,
	                                   IP_PROTO_ICMPV6, buf, rs_len);

	send_ipv6(&all_routers, IP_PROTO_ICMPV6, buf, rs_len);
	kprintf("[ndisc] Sent Router Solicitation\n");
	return 0;
}

/* ── Router Advertisement (RFC 4861 §6.2.3) ──────────────────────────
 *
 * Sends a Router Advertisement to 'dst'.  Includes:
 *  - Source Link-layer Address option
 *  - MTU option (with the interface MTU)
 *  - Prefix Information options for each known prefix
 *
 * Returns 0 on success, negative errno on failure. */

int ipv6_nd_send_ra(const struct in6_addr *dst)
{
	uint8_t buf[512];
	struct nd_router_advert *ra;
	int offset;
	int i;

	if (!dst || !net_ipv6_ll_ready)
		return -EINVAL;

	memset(buf, 0, sizeof(buf));
	ra = (struct nd_router_advert *)buf;
	ra->icmp.type = ICMPV6_RA;
	ra->icmp.code = 0;
	ra->icmp.checksum = 0;
	ra->cur_hop_limit = 64;   /* default hop limit */
	ra->flags = 0;            /* M=0 (managed), O=0 (other) */
	ra->router_lifetime = htons(1800); /* 30 minutes */
	ra->reachable_time = htonl(30000); /* 30 seconds (milliseconds) */
	ra->retrans_timer = htonl(1000);   /* 1 second (milliseconds) */

	offset = sizeof(struct nd_router_advert);

	/* Source Link-layer Address option */
	{
		struct nd_option *opt = (struct nd_option *)(buf + offset);
		opt->type = ND_OPT_SRC_LLADDR;
		opt->len  = 1;
		memcpy(buf + offset + 2, net_our_mac, 6);
		offset += 8;
	}

	/* MTU option */
	{
		struct nd_option *opt = (struct nd_option *)(buf + offset);
		opt->type = ND_OPT_MTU;
		opt->len  = 1;
		/* MTU value starts at offset 2 within option, 4 bytes */
				uint32_t mtu = htonl(net_ipv6_link_mtu);
		memcpy(buf + offset + 2, &mtu, 4);
		offset += 8;
	}

	/* Prefix Information options for each known prefix */
	for (i = 0; i < ND_PREFIX_LIST_SIZE; i++) {
		struct nd_prefix_entry *pe = &nd_prefixes[i];

		if (!pe->valid)
			continue;

		if (offset + 32 > (int)sizeof(buf))
			break;

		uint8_t *pi = buf + offset;
		pi[0] = ND_OPT_PREFIX_INFO;    /* type */
		pi[1] = 4;                      /* length in 8-octet units = 32 bytes */
		pi[2] = pe->prefix_len;        /* prefix length */
		pi[3] = pe->flags;             /* L=bit7, A=bit6 */
		/* Valid lifetime (4 bytes) */
		uint32_t vl = htonl(pe->valid_lifetime);
		memcpy(pi + 4, &vl, 4);
		/* Preferred lifetime (4 bytes) */
		uint32_t pl = htonl(pe->preferred_lifetime);
		memcpy(pi + 8, &pl, 4);
		/* Reserved (4 bytes) */
		pi[12] = pi[13] = pi[14] = pi[15] = 0;
		/* Prefix (16 bytes) */
		memcpy(pi + 16, &pe->prefix, 16);
		offset += 32;
	}

	/* Compute ICMPv6 checksum over entire message */
	{
		uint16_t ra_len = (uint16_t)offset;
		ra->icmp.checksum = ipv6_checksum(&net_our_ipv6_ll, dst,
		                                   IP_PROTO_ICMPV6, buf, ra_len);
		send_ipv6(dst, IP_PROTO_ICMPV6, buf, ra_len);
	}

	kprintf("[ndisc] Sent Router Advertisement to %02x%02x:...\n",
	        dst->s6_addr[0], dst->s6_addr[1]);
	return 0;
}

/* ── Handle incoming Router Solicitation (RFC 4861 §6.2.6) ──────────
 *
 * When we receive an RS (as a router), we respond with an RA.
 * Extract the source MAC from the Source Link-layer Address option
 * and add it to the neighbor cache. */

void ipv6_nd_handle_rs(struct ipv6_header *ip6,
                        const uint8_t *payload, uint16_t len)
{
	const struct nd_router_solicit *rs;
	const struct nd_option *opt;
	int opt_offset;

	(void)rs;

	if (!ip6 || !payload || len < sizeof(struct nd_router_solicit))
		return;

	rs = (const struct nd_router_solicit *)payload;

	/* We only respond if we have a link-local address */
	if (!net_ipv6_ll_ready)
		return;

	/* Extract source MAC from Source Link-layer Address option */
	opt = (const struct nd_option *)(payload + sizeof(struct nd_router_solicit));
	opt_offset = sizeof(struct nd_router_solicit);

	while (opt_offset + 2 <= (int)len) {
		if (opt->type == ND_OPT_SRC_LLADDR && opt->len == 1) {
			ipv6_nd_cache_add(&ip6->src_ip,
			                  payload + opt_offset + 2);
			break;
		}
		opt_offset += (int)opt->len * 8;
		if (opt_offset + 2 > (int)len)
			break;
		opt = (const struct nd_option *)(payload + opt_offset);
	}

	/* Respond with a Router Advertisement (unicast to the sender) */
	if (!ipv6_addr_is_unspecified(&ip6->src_ip)) {
		ipv6_nd_send_ra(&ip6->src_ip);
	} else {
		/* Src is unspecified — sender doing DAD, send to all-nodes */
		struct in6_addr all_nodes = IPV6_ADDR_ALL_NODES;
		ipv6_nd_send_ra(&all_nodes);
	}
}

/* ── Handle incoming Router Advertisement (RFC 4861 §6.2.5) ─────────
 *
 * Processes Router Advertisements for SLAAC, default gateway
 * discovery, route information, and DNS configuration.
 * Supports:
 *  - Prefix Information option (RFC 4861 §4.6.2)
 *  - MTU option (RFC 4861 §4.6.4)
 *  - Route Information option (RFC 4191 §2.3)
 *  - RDNSS option (RFC 8106 §5.1) */

void ipv6_nd_handle_ra(struct ipv6_header *ip6,
                        const uint8_t *payload, uint16_t len)
{
	const struct nd_router_advert *ra;
	struct nd_router_entry *re = NULL;
	const uint8_t *opt_data;
	int remaining;
	int i;

	if (!ip6 || !payload || len < sizeof(struct nd_router_advert))
		return;

	if (!net_ipv6_ll_ready)
		return;

	ra = (const struct nd_router_advert *)payload;

	kprintf("[ndisc] Received Router Advertisement (lifetime=%u, "
	        "reachable=%u, retrans=%u)\n",
	        ntohs(ra->router_lifetime),
	        ntohl(ra->reachable_time),
	        ntohl(ra->retrans_timer));

	/* Update or add default router entry */
	for (i = 0; i < ND_ROUTER_LIST_SIZE; i++) {
		if (nd_routers[i].valid &&
		    ipv6_addr_equal(&nd_routers[i].addr, &ip6->src_ip)) {
			re = &nd_routers[i];
			break;
		}
	}

	if (!re) {
		/* Find free slot */
		for (i = 0; i < ND_ROUTER_LIST_SIZE; i++) {
			if (!nd_routers[i].valid) {
				re = &nd_routers[i];
				break;
			}
		}
		/* Replace oldest if full */
		if (!re) {
			uint64_t oldest = UINT64_MAX;
			for (i = 0; i < ND_ROUTER_LIST_SIZE; i++) {
				if (nd_routers[i].expiry_tick < oldest) {
					oldest = nd_routers[i].expiry_tick;
					re = &nd_routers[i];
				}
			}
		}
	}

	if (re) {
		uint16_t lifetime = ntohs(ra->router_lifetime);
		memcpy(&re->addr, &ip6->src_ip, sizeof(struct in6_addr));
		re->lifetime = lifetime;
		if (lifetime > 0)
			re->expiry_tick = timer_get_ticks() + (uint64_t)lifetime * 100;
		else
			re->expiry_tick = 0;
		re->valid = 1;
		if (!nd_router_count)
			nd_router_count++;
	}

	/* Update the global default gateway */
	if (ntohs(ra->router_lifetime) > 0) {
		memcpy(&net_ipv6_gateway, &ip6->src_ip, sizeof(struct in6_addr));
		kprintf("[ndisc] Default gateway set to router %02x%02x:...\n",
		        ip6->src_ip.s6_addr[0], ip6->src_ip.s6_addr[1]);
	}

	/* Process options */
	opt_data = payload + sizeof(struct nd_router_advert);
	remaining = (int)(len - sizeof(struct nd_router_advert));

	while (remaining >= 2) {
		const struct nd_option *opt;
		int opt_len;

		opt = (const struct nd_option *)opt_data;
		opt_len = (int)opt->len * 8;

		if (opt_len <= 0 || opt_len > remaining)
			break;

		switch (opt->type) {

		case ND_OPT_PREFIX_INFO:
			if (opt_len >= 32) {
				uint8_t prefix_len = opt_data[2];
				uint8_t prefix_flags = opt_data[3];
				uint32_t valid_lifetime = ((uint32_t)opt_data[4] << 24) |
				                          ((uint32_t)opt_data[5] << 16) |
				                          ((uint32_t)opt_data[6] << 8)  |
				                           (uint32_t)opt_data[7];
				uint32_t preferred_lifetime = ((uint32_t)opt_data[8] << 24) |
				                               ((uint32_t)opt_data[9] << 16) |
				                               ((uint32_t)opt_data[10] << 8) |
				                                (uint32_t)opt_data[11];
				const uint8_t *prefix_bytes = opt_data + 16;

				/* Add/update prefix in our prefix table */
				struct nd_prefix_entry *pe = NULL;
				int found = 0;

				for (i = 0; i < ND_PREFIX_LIST_SIZE; i++) {
					if (!nd_prefixes[i].valid)
						continue;
					if (nd_prefixes[i].prefix_len == prefix_len &&
					    memcmp(&nd_prefixes[i].prefix, prefix_bytes, 16) == 0) {
						pe = &nd_prefixes[i];
						found = 1;
						break;
					}
				}

				if (!pe) {
					for (i = 0; i < ND_PREFIX_LIST_SIZE; i++) {
						if (!nd_prefixes[i].valid) {
							pe = &nd_prefixes[i];
							break;
						}
					}
				}

				if (pe) {
					memcpy(&pe->prefix, prefix_bytes, 16);
					pe->prefix_len = prefix_len;
					pe->valid_lifetime = valid_lifetime;
					pe->preferred_lifetime = preferred_lifetime;
					pe->flags = prefix_flags;
					if (valid_lifetime == 0xFFFFFFFF || valid_lifetime == 0)
						pe->expiry_tick = UINT64_MAX;
					else
						pe->expiry_tick = timer_get_ticks() +
						                  (uint64_t)valid_lifetime * 100;
					pe->valid = 1;
					if (!found)
						nd_prefix_count++;

					/* Autoconfigure GUA if A-flag is set */
					if (prefix_flags & 0x40) {
						struct in6_addr gua;
						memset(&gua, 0, sizeof(gua));

						if (prefix_len == 64) {
							memcpy(gua.s6_addr, prefix_bytes, 8);
							memcpy(gua.s6_addr + 8,
							       net_our_ipv6_ll.s6_addr + 8, 8);
						} else {
							memcpy(&gua, prefix_bytes, 16);
							if (prefix_len < 128) {
								int byte = prefix_len / 8;
								int bit  = prefix_len % 8;
								if (byte < 16) {
									gua.s6_addr[byte] &=
									    (uint8_t)(0xFF << (8 - bit));
									for (int j = byte + 1; j < 16; j++)
										gua.s6_addr[j] = 0;
								}
							}
							for (int j = 8; j < 16; j++)
								gua.s6_addr[j] |=
								    net_our_ipv6_ll.s6_addr[j];
						}

						memcpy(&net_our_ipv6_gua, &gua,
					               sizeof(struct in6_addr));
						/* net_ipv6_gua_valid will be set by DAD */

						ipv6_addr_add(&gua, prefix_len,
						              IPV6_ADDR_STATE_TENTATIVE,
						              valid_lifetime, preferred_lifetime,
						              IPV6_ADDR_F_AUTOCONF | IPV6_ADDR_F_DAD);

						ipv6_dad_start(&gua);

						kprintf("[ndisc] SLAAC: configured GUA "
						        "(tentative, DAD in progress) "
						        "valid=%u preferred=%u\n",
						        valid_lifetime, preferred_lifetime);
					}
				}
			}
			break;

		case ND_OPT_MTU:
			if (opt_len >= 8) {
				uint32_t mtu = ((uint32_t)opt_data[2] << 24) |
				               ((uint32_t)opt_data[3] << 16) |
				               ((uint32_t)opt_data[4] << 8)  |
				                (uint32_t)opt_data[5];
				kprintf("[ndisc] RA MTU option: %u (link MTU updated)\n", mtu);
				net_ipv6_link_mtu = mtu;
			}
			break;

		case ND_OPT_ROUTE_INFO:
			if (opt_len >= 8) {
				uint8_t prefix_len = opt_data[2];
				uint8_t route_flags = opt_data[3];   /* prf (2 bits) */
				uint32_t route_lifetime = ((uint32_t)opt_data[4] << 24) |
				                          ((uint32_t)opt_data[5] << 16) |
				                          ((uint32_t)opt_data[6] << 8)  |
				                           (uint32_t)opt_data[7];
				/* Prefix follows at offset 8, up to 16 bytes */
				kprintf("[ndisc] RA Route Info: %d/%d lifetime=%u "
				        "pref=%d\n",
				        prefix_len, prefix_len, route_lifetime,
				        (route_flags >> 6) & 0x03);
			}
			break;

		case ND_OPT_RDNSS:
			if (opt_len >= 8) {
				/* RDNSS option format (RFC 8106 §5.1):
				 *   type=25 (1 byte)
				 *   len (1 byte)
				 *   reserved (2 bytes)
				 *   lifetime (4 bytes)
				 *   addresses (variable, 16 bytes each) */
				uint32_t lifetime = ((uint32_t)opt_data[4] << 24) |
				                    ((uint32_t)opt_data[5] << 16) |
				                    ((uint32_t)opt_data[6] << 8)  |
				                     (uint32_t)opt_data[7];
				int addr_count = (opt_len - 8) / 16;

				if (addr_count >= 1) {
					/* Take the first DNS server address */
					memcpy(&net_ipv6_dns, opt_data + 8,
					       sizeof(struct in6_addr));
					kprintf("[ndisc] RA RDNSS: DNS server set, "
					        "lifetime=%u\n", lifetime);
				}
			}
			break;

		default:
			/* Unknown option — skip per RFC 4861 §4.1 */
			break;
		}

		opt_data += opt_len;
		remaining -= opt_len;
	}
}

/* ── RA prefix lifetime expiry check ─────────────────────────────────
 *
 * Called from ipv6_nd_poll() to expire stale prefixes.
 */
static void nd_expire_prefixes(void)
{
	uint64_t now = timer_get_ticks();
	int i;

	for (i = 0; i < ND_PREFIX_LIST_SIZE; i++) {
		struct nd_prefix_entry *pe = &nd_prefixes[i];

		if (!pe->valid)
			continue;
		if (pe->expiry_tick == UINT64_MAX)
			continue;
		if (now >= pe->expiry_tick) {
			kprintf("[ndisc] Prefix %02x%02x:.../%d expired\n",
			        pe->prefix.s6_addr[0], pe->prefix.s6_addr[1],
			        pe->prefix_len);
			memset(pe, 0, sizeof(*pe));
			nd_prefix_count--;
		}
	}
}

/* ── Default router expiry check ─────────────────────────────────────
 *
 * Called from ipv6_nd_poll() to expire stale router entries.
 */
static void nd_expire_routers(void)
{
	uint64_t now = timer_get_ticks();
	int i;

	for (i = 0; i < ND_ROUTER_LIST_SIZE; i++) {
		struct nd_router_entry *re = &nd_routers[i];

		if (!re->valid)
			continue;
		if (re->expiry_tick == 0)
			continue;
		if (now >= re->expiry_tick) {
			kprintf("[ndisc] Default router %02x%02x:... expired\n",
			        re->addr.s6_addr[0], re->addr.s6_addr[1]);
			memset(re, 0, sizeof(*re));
			nd_router_count--;
		}
	}
}

/* ── Update ipv6_nd_poll with RS/RA expiry checks ─────────────────── */

/* The original ipv6_nd_poll now also checks prefix/router expiry */

void ipv6_nd_cache_dump(void)
{
	int i;

	kprintf("[ndisc] Neighbour cache (%d entries):\n", ND_CACHE_SIZE);
	for (i = 0; i < ND_CACHE_SIZE; i++) {
		struct nd_cache_entry *e = &nd_cache[i];
		const char *state_str;
		static const char * const states[] = {
			"INCOMPLETE", "REACHABLE", "STALE",
			"DELAY", "PROBE"
		};

		if (!e->valid)
			continue;

		if (e->state >= 0 && e->state <= ND_REACH_PROBE)
			state_str = states[e->state];
		else
			state_str = "?";

		kprintf("  [%d] %02x%02x:%02x%02x:%02x%02x:%02x%02x:"
		        "%02x%02x:%02x%02x:%02x%02x:%02x%02x "
		        "→ %02x:%02x:%02x:%02x:%02x:%02x "
		        "[%s]%s\n",
		        i,
		        e->ip6.s6_addr[0], e->ip6.s6_addr[1],
		        e->ip6.s6_addr[2], e->ip6.s6_addr[3],
		        e->ip6.s6_addr[4], e->ip6.s6_addr[5],
		        e->ip6.s6_addr[6], e->ip6.s6_addr[7],
		        e->ip6.s6_addr[8], e->ip6.s6_addr[9],
		        e->ip6.s6_addr[10], e->ip6.s6_addr[11],
		        e->ip6.s6_addr[12], e->ip6.s6_addr[13],
		        e->ip6.s6_addr[14], e->ip6.s6_addr[15],
		        e->mac[0], e->mac[1], e->mac[2],
		        e->mac[3], e->mac[4], e->mac[5],
		        state_str,
		        e->is_router ? " R" : "");
	}
}
