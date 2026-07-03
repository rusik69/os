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
	kprintf("[ndisc] IPv6 Neighbor Discovery initialised\n");
}

/* ── Cache dump (debugging) ──────────────────────────────────────── */

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
