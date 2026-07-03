/*
 * ipv6_pmtu.c — IPv6 Path MTU Discovery (RFC 1981)
 *
 * Implements PMTU cache management:
 *  - Cache of <destination, PMTU> entries
 *  - Periodic refresh (re-probe) to detect increased path MTU
 *  - ICMPv6 Packet Too Big sending (RFC 4443 §3.2)
 *
 * Integration points:
 *  - ipv6_pmtu_lookup() called from send_ipv6() before fragmenting
 *  - ipv6_pmtu_update() called from ICMPv6 type 2 handler
 *  - ipv6_pmtu_poll() called from ipv6_poll() periodically
 *  - ipv6_send_pmtu() called when forwarding fails due to MTU
 */

#define KERNEL_INTERNAL
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "errno.h"

/* ── Constants ────────────────────────────────────────────────────── */

/* Default link MTU for IPv6 over Ethernet (1500 - 40 = 1460) */
#define IPV6_DEFAULT_LINK_MTU   1460
/* Minimum IPv6 MTU per RFC 8200 §5 */
#define IPV6_MIN_MTU            1280
/* PMTU cache size */
#define IPV6_PMTU_CACHE_SIZE    8
/* PMTU re-probe timeout: 10 minutes at TIMER_FREQ=100 Hz */
#define IPV6_PMTU_REPROBE_TICKS (600 * TIMER_FREQ)
/* PMTU entry timeout for unreachable destinations: 30 minutes */
#define IPV6_PMTU_ENTRY_TICKS   (1800 * TIMER_FREQ)

/* ── PMTU cache entry ─────────────────────────────────────────────── */

struct ipv6_pmtu_entry {
	struct in6_addr dst;           /* destination address */
	uint16_t        pmtu;          /* discovered path MTU */
	uint64_t        update_tick;   /* last update tick */
	int             valid;         /* 1 = entry in use */
};

static struct ipv6_pmtu_entry ipv6_pmtu_cache[IPV6_PMTU_CACHE_SIZE];

/* ── PMTU cache operations ────────────────────────────────────────── */

void ipv6_pmtu_init(void)
{
	memset(ipv6_pmtu_cache, 0, sizeof(ipv6_pmtu_cache));
}

/*
 * Look up PMTU for a given destination.
 * Returns the path MTU in bytes, or 0 if no cached PMTU exists
 * (caller should use the default link MTU).
 */
uint16_t ipv6_pmtu_lookup(const struct in6_addr *dst)
{
	int i;
	uint64_t now = timer_get_ticks();

	if (!dst)
		return 0;

	for (i = 0; i < IPV6_PMTU_CACHE_SIZE; i++) {
		struct ipv6_pmtu_entry *e = &ipv6_pmtu_cache[i];
		if (!e->valid)
			continue;
		if (!ipv6_addr_equal(&e->dst, dst))
			continue;

		/*
		 * If the entry has aged past the re-probe timeout, expire it
		 * so the caller will re-probe with the default link MTU.
		 */
		if (now - e->update_tick >= IPV6_PMTU_REPROBE_TICKS) {
			e->valid = 0;
			return 0;
		}

		return e->pmtu;
	}

	return 0; /* not cached */
}

/*
 * Update the PMTU cache for a given destination.
 * Called when we receive an ICMPv6 Packet Too Big message.
 * pmtu is the new (smaller) MTU value from the ICMPv6 error.
 */
void ipv6_pmtu_update(const struct in6_addr *dst, uint16_t pmtu)
{
	struct ipv6_pmtu_entry *victim = NULL;
	int i;

	if (!dst || pmtu < IPV6_MIN_MTU)
		pmtu = IPV6_MIN_MTU;

	uint64_t now = timer_get_ticks();

	/* Try to find an existing entry for this destination */
	for (i = 0; i < IPV6_PMTU_CACHE_SIZE; i++) {
		struct ipv6_pmtu_entry *e = &ipv6_pmtu_cache[i];
		if (!e->valid)
			continue;
		if (ipv6_addr_equal(&e->dst, dst)) {
			/*
			 * Update existing entry with the smaller PMTU.
			 * RFC 1981 §5.2: PMTU can only decrease via ICMPv6;
			 * increases are detected by re-probing.
			 */
			if (pmtu < e->pmtu) {
				e->pmtu = pmtu;
				e->update_tick = now;
				kprintf("[ipv6_pmtu] reduced PMTU to %u for destination\n",
					pmtu);
			}
			return;
		}
	}

	/* Find a free slot */
	for (i = 0; i < IPV6_PMTU_CACHE_SIZE; i++) {
		struct ipv6_pmtu_entry *e = &ipv6_pmtu_cache[i];
		if (!e->valid) {
			victim = e;
			break;
		}
	}

	/* If no free slot, evict the oldest entry */
	if (!victim) {
		uint64_t oldest_tick = now;
		for (i = 0; i < IPV6_PMTU_CACHE_SIZE; i++) {
			struct ipv6_pmtu_entry *e = &ipv6_pmtu_cache[i];
			if (e->valid && e->update_tick < oldest_tick) {
				oldest_tick = e->update_tick;
				victim = e;
			}
		}
	}

	if (victim) {
		memset(victim, 0, sizeof(*victim));
		memcpy(&victim->dst, dst, sizeof(struct in6_addr));
		victim->pmtu = pmtu;
		victim->update_tick = now;
		victim->valid = 1;
		kprintf("[ipv6_pmtu] cached PMTU %u for new destination\n", pmtu);
	}
}

/*
 * Expire stale PMTU entries.
 * Called periodically from ipv6_poll().
 */
void ipv6_pmtu_poll(void)
{
	uint64_t now = timer_get_ticks();
	int i;

	for (i = 0; i < IPV6_PMTU_CACHE_SIZE; i++) {
		struct ipv6_pmtu_entry *e = &ipv6_pmtu_cache[i];
		if (!e->valid)
			continue;
		/* Entries older than IPV6_PMTU_ENTRY_TICKS are fully expired */
		if (now - e->update_tick >= IPV6_PMTU_ENTRY_TICKS) {
			e->valid = 0;
			kprintf("[ipv6_pmtu] expired PMTU entry (no recent activity)\n");
		}
		/*
		 * Entries that exceed IPV6_PMTU_REPROBE_TICKS are handled
		 * in ipv6_pmtu_lookup() — they're expired on next send,
		 * allowing a re-probe with the default link MTU.
		 */
	}
}

/*
 * Send ICMPv6 Packet Too Big (RFC 4443 §3.2).
 *
 * Sent when a packet cannot be forwarded because it exceeds the MTU of
 * the next-hop link.  The ICMPv6 body contains the MTU of the
 * constricting link (32-bit) followed by as much of the offending
 * packet as will fit within the IPv6 minimum MTU (1280 bytes).
 *
 * Structure:
 *   struct icmpv6_header (type=2, code=0)
 *   uint32_t mtu;           // MTU of the constricting link
 *   uint8_t  offending[];   // portion of the original packet
 */
void ipv6_send_pmtu(const struct in6_addr *src,
                     const struct in6_addr *dst,
                     const struct ipv6_header *offending,
                     uint16_t offending_len,
                     uint32_t next_hop_mtu)
{
	uint8_t buf[1280];
	struct icmpv6_header *icmp = (struct icmpv6_header *)buf;
	uint32_t *mtu_field;
	uint16_t data_offset;
	uint16_t copy_len;
	uint16_t icmp_len;

	memset(buf, 0, sizeof(buf));

	icmp->type = ICMPV6_PACKET_TOO_BIG;
	icmp->code = 0;
	icmp->checksum = 0;

	/* MTU field follows immediately after the ICMPv6 header */
	mtu_field = (uint32_t *)(buf + sizeof(struct icmpv6_header));
	*mtu_field = htonl(next_hop_mtu);

	/* Copy as much of the offending packet as fits in 1280 bytes */
	data_offset = (uint16_t)(sizeof(struct icmpv6_header) + sizeof(uint32_t));
	copy_len = offending_len;
	if (data_offset + copy_len > sizeof(buf))
		copy_len = (uint16_t)(sizeof(buf) - data_offset);

	if (copy_len > 0)
		memcpy(buf + data_offset, offending, copy_len);

	icmp_len = data_offset + copy_len;

	icmp->checksum = ipv6_checksum(src, dst,
	                                IP_PROTO_ICMPV6,
	                                buf, icmp_len);

	kprintf("[ipv6_pmtu] sending Packet Too Big: MTU=%u\n", next_hop_mtu);

	send_ipv6(dst, IP_PROTO_ICMPV6, buf, icmp_len);
}
