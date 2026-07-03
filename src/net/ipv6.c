/*
 * ipv6.c — IPv6 basic stateless auto-configuration (SLAAC)
 *
 * Implements:
 *  - Link-local address generation (FE80::/10 via modified EUI-64)
 *  - ICMPv6 Neighbor Discovery (NS/NA)
 *  - Router Solicitation on startup
 *  - Router Advertisement processing for SLAAC
 *  - IPv6 packet dispatch
 *
 * This is a minimal production-quality IPv6 stack suitable for
 * a hobby OS. It covers RFC 4861 (NDP) and RFC 4862 (SLAAC).
 */

#define KERNEL_INTERNAL
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "rng.h"

/* ── IPv6 state ──────────────────────────────────────────────────── */

struct in6_addr net_our_ipv6_ll;    /* link-local address */
struct in6_addr net_our_ipv6_gua;   /* global unicast (via SLAAC) */
int  net_ipv6_ll_ready   = 0;       /* 1 = link-local address configured */
int  net_ipv6_gua_valid  = 0;       /* 1 = GUA configured via SLAAC */
struct in6_addr net_ipv6_gateway;   /* default gateway (from RA) */
struct in6_addr net_ipv6_dns;       /* DNS server (from RDNSS) */
uint32_t net_ipv6_ns_count = 0;     /* NS counter for duplicate detection */
uint32_t net_ipv6_link_mtu = 1500;  /* link MTU (Ethernet default = 1500) */

/* ── IPv6 address table ───────────────────────────────────────── */
struct ipv6_addr_entry ipv6_addr_table[IPV6_ADDR_TABLE_SIZE];
int ipv6_addr_count = 0;

/* RS retry state */
static int  rs_sent = 0;            /* have we sent a Router Solicitation? */
static int  rs_retries = 0;         /* RS retry count */
static uint64_t rs_last_tick = 0;   /* tick of last RS transmission */
#define RS_MAX_RETRIES      3
#define RS_RETRY_INTERVAL   50      /* 500 ms (at TIMER_FREQ=100 Hz) */

/* Neighbor Cache uses ipv6_ndisc.c module */

/* ── EUI-64 from MAC address (RFC 4291 appendix A) ───────────────── */
/* MAC 00:11:22:33:44:55 → EUI-64 02:11:22:FF:FE:33:44:55
 * (invert the Universal/Local bit, insert FF:FE in the middle) */
void ipv6_eui64_from_mac(const uint8_t *mac, struct in6_addr *out)
{
    out->s6_addr[0] = mac[0] ^ 0x02;  /* invert U/L bit */
    out->s6_addr[1] = mac[1];
    out->s6_addr[2] = mac[2];
    out->s6_addr[3] = 0xFF;
    out->s6_addr[4] = 0xFE;
    out->s6_addr[5] = mac[3];
    out->s6_addr[6] = mac[4];
    out->s6_addr[7] = mac[5];
    /* Upper 64 bits (subnet prefix) are set by the caller */
}

/* ── Address helpers ─────────────────────────────────────────────── */

int ipv6_addr_is_multicast(const struct in6_addr *addr)
{
    return addr->s6_addr[0] == 0xFF;
}

int ipv6_addr_is_linklocal(const struct in6_addr *addr)
{
    return (addr->s6_addr[0] == 0xFE) && ((addr->s6_addr[1] & 0xC0) == 0x80);
}

int ipv6_addr_is_unspecified(const struct in6_addr *addr)
{
    for (int i = 0; i < 16; i++) {
        if (addr->s6_addr[i] != 0) return 0;
    }
    return 1;
}

int ipv6_addr_equal(const struct in6_addr *a, const struct in6_addr *b)
{
    return memcmp(a->s6_addr, b->s6_addr, 16) == 0;
}

/* ── Address scope ─────────────────────────────────────────────── */

/* Determine the scope of an IPv6 address per RFC 6724 §3.1.
 * Returns a scope value; lower means more specific. */
int ipv6_addr_get_scope(const struct in6_addr *addr)
{
    if (!addr) return 0;

    /* Unspecified (::) */
    if (ipv6_addr_is_unspecified(addr))
        return 0;

    /* Loopback (::1) — interface-local */
    if (addr->s6_addr[0] == 0 && addr->s6_addr[1] == 0 &&
        addr->s6_addr[2] == 0 && addr->s6_addr[3] == 0 &&
        addr->s6_addr[4] == 0 && addr->s6_addr[5] == 0 &&
        addr->s6_addr[6] == 0 && addr->s6_addr[7] == 0 &&
        addr->s6_addr[8] == 0 && addr->s6_addr[9] == 0 &&
        addr->s6_addr[10] == 0 && addr->s6_addr[11] == 0 &&
        addr->s6_addr[12] == 0 && addr->s6_addr[13] == 0 &&
        addr->s6_addr[14] == 0 && addr->s6_addr[15] == 1)
        return 0x01;

    /* Link-local (FE80::/10) */
    if ((addr->s6_addr[0] == 0xFE) && ((addr->s6_addr[1] & 0xC0) == 0x80))
        return 0x02;

    /* Multicast (FF00::/8) — scope is in bits 4-7 of byte 1 */
    if (addr->s6_addr[0] == 0xFF)
        return (addr->s6_addr[1] & 0x0F);

    /* Unique-local (FC00::/7) */
    if ((addr->s6_addr[0] & 0xFE) == 0xFC)
        return 0x08;

    /* Global unicast (the rest) */
    return 0x0E;
}

/* ── Address management infrastructure ────────────────────────── */

/* Add an IPv6 address to the address table.
 * Returns 0 on success, negative errno on error. */
int ipv6_addr_add(const struct in6_addr *addr, uint8_t prefix_len,
                   int state, uint32_t valid_lifetime,
                   uint32_t preferred_lifetime, uint32_t flags)
{
    struct ipv6_addr_entry *entry;
    int slot = -1;
    int i;

    if (!addr)
        return -EINVAL;

    /* Check if address already exists — update instead */
    entry = ipv6_addr_find(addr);
    if (entry) {
        entry->prefix_len = prefix_len;
        entry->state = state;
        entry->valid_lifetime = valid_lifetime;
        entry->preferred_lifetime = preferred_lifetime;
        entry->flags = flags;
        entry->scope = ipv6_addr_get_scope(addr);
        if (valid_lifetime == 0xFFFFFFFF)
            entry->expiry_tick = UINT64_MAX;
        else
            entry->expiry_tick = timer_get_ticks() + (uint64_t)valid_lifetime * 100;
        if (preferred_lifetime == 0xFFFFFFFF || preferred_lifetime == 0)
            entry->preferred_expiry_tick = UINT64_MAX;
        else
            entry->preferred_expiry_tick = timer_get_ticks() + (uint64_t)preferred_lifetime * 100;
        entry->valid = 1;
        return 0;
    }

    /* Find free slot */
    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        if (!ipv6_addr_table[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -ENOSPC;

    /* Populate entry */
    entry = &ipv6_addr_table[slot];
    memcpy(&entry->addr, addr, sizeof(struct in6_addr));
    entry->prefix_len = prefix_len;
    entry->state = state;
    entry->scope = ipv6_addr_get_scope(addr);
    entry->valid_lifetime = valid_lifetime;
    entry->preferred_lifetime = preferred_lifetime;
    if (valid_lifetime == 0xFFFFFFFF)
        entry->expiry_tick = UINT64_MAX;
    else
        entry->expiry_tick = timer_get_ticks() + (uint64_t)valid_lifetime * 100;
    if (preferred_lifetime == 0xFFFFFFFF || preferred_lifetime == 0)
        entry->preferred_expiry_tick = UINT64_MAX;
    else
        entry->preferred_expiry_tick = timer_get_ticks() + (uint64_t)preferred_lifetime * 100;
    entry->flags = flags;
    entry->valid = 1;
    ipv6_addr_count++;
    return 0;
}

/* Remove an IPv6 address from the table.
 * Returns 0 on success, -ENOENT if not found. */
int ipv6_addr_del(const struct in6_addr *addr)
{
    int i;

    if (!addr)
        return -EINVAL;

    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        if (!ipv6_addr_table[i].valid)
            continue;
        if (ipv6_addr_equal(&ipv6_addr_table[i].addr, addr)) {
            memset(&ipv6_addr_table[i], 0, sizeof(struct ipv6_addr_entry));
            ipv6_addr_count--;
            return 0;
        }
    }
    return -ENOENT;
}

/* Find an address entry by address.
 * Returns pointer to entry, or NULL if not found. */
struct ipv6_addr_entry *ipv6_addr_find(const struct in6_addr *addr)
{
    int i;

    if (!addr)
        return NULL;

    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        if (!ipv6_addr_table[i].valid)
            continue;
        if (ipv6_addr_equal(&ipv6_addr_table[i].addr, addr))
            return &ipv6_addr_table[i];
    }
    return NULL;
}

/* Find first address entry with a given state.
 * Returns pointer to entry, or NULL if none. */
struct ipv6_addr_entry *ipv6_addr_find_by_state(int state)
{
    int i;

    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        if (!ipv6_addr_table[i].valid)
            continue;
        if (ipv6_addr_table[i].state == state)
            return &ipv6_addr_table[i];
    }
    return NULL;
}

/* Simplified source address selection (RFC 6724).
 *
 * Rules applied (simplified):
 * 1. Prefer same scope as destination
 * 2. Prefer preferred state over deprecated
 * 3. Prefer permanent over temporary
 *
 * Returns pointer to selected entry, or NULL if none available. */
struct ipv6_addr_entry *ipv6_addr_select_source(const struct in6_addr *dst)
{
    int dst_scope;
    struct ipv6_addr_entry *best = NULL;
    int best_score = -1;
    int i;

    if (!dst)
        goto fallback;

    dst_scope = ipv6_addr_get_scope(dst);

    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        struct ipv6_addr_entry *e = &ipv6_addr_table[i];
        int score = 0;

        if (!e->valid)
            continue;
        if (e->state == IPV6_ADDR_STATE_TENTATIVE ||
            e->state == IPV6_ADDR_STATE_DETACHED)
            continue;

        /* Rule 1: Prefer same scope */
        if (e->scope == dst_scope)
            score += 100;

        /* Rule 2: Prefer preferred over deprecated */
        if (e->state == IPV6_ADDR_STATE_PREFERRED)
            score += 50;
        else if (e->state == IPV6_ADDR_STATE_PERMANENT)
            score += 40;

        /* Rule 3: Deprecated is still usable but lower priority */
        if (e->state == IPV6_ADDR_STATE_DEPRECATED)
            score += 10;

        if (score > best_score) {
            best_score = score;
            best = e;
        }
    }

    if (best)
        return best;

fallback:
    /* Fallback: return first valid non-tentative address (usually link-local) */
    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        if (!ipv6_addr_table[i].valid)
            continue;
        if (ipv6_addr_table[i].state == IPV6_ADDR_STATE_TENTATIVE ||
            ipv6_addr_table[i].state == IPV6_ADDR_STATE_DETACHED)
            continue;
        return &ipv6_addr_table[i];
    }
    return NULL;
}

/* Check if an address belongs to us.
 * Searches the address table. */
int ipv6_addr_is_ours(const struct in6_addr *addr)
{
    if (!addr) return 0;
    return ipv6_addr_find(addr) != NULL;
}

/* Get our link-local address.
 * Returns 0 on success, -1 if not configured. */
int ipv6_addr_get_ll(struct in6_addr *out)
{
    if (!out) return -1;
    if (!net_ipv6_ll_ready) return -1;
    memcpy(out, &net_our_ipv6_ll, sizeof(struct in6_addr));
    return 0;
}

/* Get our global unicast address (if configured).
 * Returns 0 on success, -1 if not configured. */
int ipv6_addr_get_gua(struct in6_addr *out)
{
    if (!out) return -1;
    if (!net_ipv6_gua_valid) return -1;
    memcpy(out, &net_our_ipv6_gua, sizeof(struct in6_addr));
    return 0;
}

/* Dump address table for debugging. */
void ipv6_addr_dump(void)
{
    int i;

    kprintf("[IPv6] Address table (%d entries):\n", ipv6_addr_count);
    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        struct ipv6_addr_entry *e = &ipv6_addr_table[i];
        if (!e->valid) continue;

        kprintf("  [%d] ", i);
        /* Print address in hex format */
        {
            const uint8_t *a = e->addr.s6_addr;
            kprintf("%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                    a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                    a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
        }
        kprintf("/%d", e->prefix_len);

        /* State string */
        switch (e->state) {
        case IPV6_ADDR_STATE_TENTATIVE:  kprintf(" tentative"); break;
        case IPV6_ADDR_STATE_PREFERRED:  kprintf(" preferred"); break;
        case IPV6_ADDR_STATE_DEPRECATED: kprintf(" deprecated"); break;
        case IPV6_ADDR_STATE_PERMANENT:  kprintf(" permanent"); break;
        case IPV6_ADDR_STATE_DETACHED:   kprintf(" detached"); break;
        default:                         kprintf(" state=%d", e->state); break;
        }

        /* Scope string */
        switch (e->scope) {
        case 0x01: kprintf(" iface"); break;
        case 0x02: kprintf(" link"); break;
        case 0x0E: kprintf(" global"); break;
        default:   kprintf(" scope=%d", e->scope); break;
        }

        if (e->flags & IPV6_ADDR_F_AUTOCONF) kprintf(" [A]");
        if (e->flags & IPV6_ADDR_F_DAD)      kprintf(" [DAD]");

        kprintf(" valid=%u pref=%u\n",
                e->valid_lifetime, e->preferred_lifetime);
    }
}

/* ── DAD (Duplicate Address Detection) — RFC 4862 §5.4 ──────────────
 *
 * Duplicate Address Detection verifies that a tentative IPv6 address
 * is not already in use on the link before assigning it to the
 * interface.
 *
 * Procedure:
 *   1. Send DupAddrDetectTransmits (3) NS probes with source = ::
 *      to the solicited-node multicast address.
 *   2. If an NA is received for the address → conflict.
 *   3. If an NS with source != :: is received for the address → conflict.
 *   4. After all probes sent without detecting a conflict → address is
 *      unique and transitions from TENTATIVE to PREFERRED/PERMANENT.
 *   5. On conflict → address transitions to DETACHED state.
 */

#define IPV6_DUPADDR_DETECT_TRANSMITS  3   /* NS probes before declaring unique */
#define DAD_RETRANS_TIMER              100  /* ticks between probes (1s at 100Hz) */

struct ipv6_dad_slot {
    struct in6_addr addr;
    int             probe_count;        /* NS probes sent so far */
    uint64_t        last_probe_tick;    /* tick when last NS was sent */
    int             active;             /* 1 = DAD in progress for this address */
    volatile int    conflict;           /* set by ipv6_dad_conflict() from NS/NA handlers */
};

static struct ipv6_dad_slot ipv6_dad_slots[IPV6_ADDR_TABLE_SIZE];

/* Send a Neighbor Solicitation for DAD with source address = ::
 * (the unspecified address).
 *
 * Per RFC 4861 §7.2.2, an NS sent for DAD MUST:
 *  - Have source = ::
 *  - Have hop limit = 255
 *  - Be sent to the solicited-node multicast address of the target
 *  - NOT include the Source Link-layer Address option
 */
static void dad_send_ns(const struct in6_addr *target)
{
    uint8_t buf[sizeof(struct ipv6_header) + sizeof(struct nd_neighbor)];
    struct ipv6_header *ip6;
    struct nd_neighbor *ns;
    struct in6_addr mcast;
    struct in6_addr unspecified;
    uint8_t eth_dst[6];
    uint16_t ns_len = sizeof(struct nd_neighbor);
    uint16_t total = sizeof(struct ipv6_header) + ns_len;

    if (!target)
        return;

    memset(buf, 0, sizeof(buf));
    ip6 = (struct ipv6_header *)buf;

    /* Build IPv6 header with source = :: (unspecified) */
    ip6->vcl_flow = htonl(0x60000000U);
    ip6->payload_length = htons(ns_len);
    ip6->next_header = IP_PROTO_ICMPV6;
    ip6->hop_limit = 255;  /* NDP requires hop limit 255 */

    /* Source stays zeroed (::) — no memcpy needed since buf is zeroed */
    ipv6_calc_solicited_node(target, &mcast);
    memcpy(&ip6->dst_ip, &mcast, sizeof(struct in6_addr));

    /* Build NS payload */
    ns = (struct nd_neighbor *)(buf + sizeof(struct ipv6_header));
    ns->icmp.type = ICMPV6_NS;
    ns->icmp.code = 0;
    ns->icmp.checksum = 0;
    ns->reserved = 0;
    memcpy(&ns->target, target, sizeof(struct in6_addr));

    /* Compute ICMPv6 checksum with unspecified source address */
    memset(&unspecified, 0, sizeof(unspecified));
    ns->icmp.checksum = ipv6_checksum(&unspecified, &mcast,
                                       IP_PROTO_ICMPV6,
                                       buf + sizeof(struct ipv6_header),
                                       ns_len);

    /* Resolve Ethernet destination (solicited-node multicast MAC) */
    eth_dst[0] = 0x33;
    eth_dst[1] = 0x33;
    eth_dst[2] = mcast.s6_addr[12];
    eth_dst[3] = mcast.s6_addr[13];
    eth_dst[4] = mcast.s6_addr[14];
    eth_dst[5] = mcast.s6_addr[15];

    send_eth_ipv6(eth_dst, buf, total);
}

/* Start DAD for a tentative address.
 *
 * The address MUST already be in the address table with state
 * IPV6_ADDR_STATE_TENTATIVE.  This function sends the first NS
 * probe and creates a DAD tracking entry.
 */
void ipv6_dad_start(const struct in6_addr *addr)
{
    int slot = -1;
    int i;

    if (!addr) return;

    /* Verify the address exists and is tentative */
    struct ipv6_addr_entry *entry = ipv6_addr_find(addr);
    if (!entry) {
        kprintf("[dad] ipv6_dad_start: address not in table\n");
        return;
    }
    if (entry->state != IPV6_ADDR_STATE_TENTATIVE) {
        /* Address already resolved — no DAD needed */
        return;
    }

    /* Check if DAD is already active for this address */
    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        if (ipv6_dad_slots[i].active &&
            ipv6_addr_equal(&ipv6_dad_slots[i].addr, addr)) {
            /* Already doing DAD — no-op */
            return;
        }
    }

    /* Find free slot */
    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        if (!ipv6_dad_slots[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        kprintf("[dad] no free DAD slot for ");
        kprintf("%02x%02x:...\n", addr->s6_addr[0], addr->s6_addr[1]);
        return;
    }

    /* Initialize DAD state */
    memcpy(&ipv6_dad_slots[slot].addr, addr, sizeof(struct in6_addr));
    ipv6_dad_slots[slot].probe_count = 0;
    ipv6_dad_slots[slot].last_probe_tick = 0;
    ipv6_dad_slots[slot].active = 1;
    ipv6_dad_slots[slot].conflict = 0;

    /* Send first NS probe */
    dad_send_ns(addr);
    ipv6_dad_slots[slot].probe_count = 1;
    ipv6_dad_slots[slot].last_probe_tick = timer_get_ticks();

    kprintf("[dad] started DAD for ");
    {
        const uint8_t *a = addr->s6_addr;
        kprintf("%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
                a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
    }
}

/* Mark a DAD conflict for an address.
 *
 * Called from ipv6_nd_handle_ns() and ipv6_nd_handle_na() when a
 * duplicate is detected.  The actual state transition to DETACHED
 * happens in ipv6_dad_poll() to avoid re-entrancy issues.
 */
void ipv6_dad_conflict(const struct in6_addr *addr)
{
    int i;
    if (!addr) return;

    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        if (ipv6_dad_slots[i].active &&
            ipv6_addr_equal(&ipv6_dad_slots[i].addr, addr)) {
            ipv6_dad_slots[i].conflict = 1;
            kprintf("[dad] CONFLICT detected for ");
            {
                const uint8_t *a = addr->s6_addr;
                kprintf("%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                        "%02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
                        a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                        a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
            }
            return;
        }
    }
}

/* Poll DAD state machine — called from ipv6_poll().
 *
 * For each active DAD entry:
 *  - If conflict detected → mark address DETACHED, clean up DAD slot
 *  - If retransmit timer expired → send next probe
 *  - If all probes sent without conflict → address is unique:
 *    transition from TENTATIVE to PREFERRED/PERMANENT
 */
void ipv6_dad_poll(void)
{
    uint64_t now = timer_get_ticks();
    int i;

    for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
        struct ipv6_dad_slot *dad = &ipv6_dad_slots[i];

        if (!dad->active)
            continue;

        /* Check if conflict was flagged by NS/NA handler */
        if (dad->conflict) {
            struct ipv6_addr_entry *entry = ipv6_addr_find(&dad->addr);
            if (entry) {
                entry->state = IPV6_ADDR_STATE_DETACHED;
                kprintf("[dad] address DETACHED due to duplicate\n");
            }
            memset(dad, 0, sizeof(*dad));
            continue;
        }

        /* Check if it's time to retransmit */
        if (now - dad->last_probe_tick < DAD_RETRANS_TIMER)
            continue;

        /* Send next probe */
        if (dad->probe_count < IPV6_DUPADDR_DETECT_TRANSMITS) {
            dad_send_ns(&dad->addr);
            dad->probe_count++;
            dad->last_probe_tick = now;
            continue;
        }

        /* All probes sent without conflict → address is unique */
        {
            struct ipv6_addr_entry *entry = ipv6_addr_find(&dad->addr);
            if (entry) {
                int old_state = entry->state;

                /* Determine new state based on flags */
                if (entry->flags & IPV6_ADDR_F_AUTOCONF) {
                    /* SLAAC address → PREFERRED */
                    entry->state = IPV6_ADDR_STATE_PREFERRED;

                    /* Set the global GUA if this is our SLAAC address */
                    if (!net_ipv6_gua_valid) {
                        memcpy(&net_our_ipv6_gua, &dad->addr,
                               sizeof(struct in6_addr));
                        net_ipv6_gua_valid = 1;
                        kprintf("[dad] GUA %02x%02x:... now valid via DAD\n",
                                dad->addr.s6_addr[0],
                                dad->addr.s6_addr[1]);
                    }
                } else {
                    /* Link-local or other → PERMANENT */
                    entry->state = IPV6_ADDR_STATE_PERMANENT;
                }

                /* Clear the DAD flag — DAD completed */
                entry->flags &= ~(uint32_t)IPV6_ADDR_F_DAD;

                kprintf("[dad] DAD complete for address "
                        "(state %d → %d, flags=0x%x)\n",
                        old_state, entry->state, entry->flags);
            }
            memset(dad, 0, sizeof(*dad));
        }
    }
}

/* ── Neighbour Cache (managed by ipv6_ndisc.c) ───────────────────── */

/* ── IPv6 pseudo-header checksum (RFC 8200, §8.1) ────────────────── */

/* Compute checksum over IPv6 pseudo-header + upper-layer data.
 * The caller must pass the source, destination, next-header type,
 * and the upper-layer data (including the upper-layer header). */
uint16_t ipv6_checksum(const struct in6_addr *src,
                        const struct in6_addr *dst,
                        uint8_t next_hdr,
                        const void *data, uint16_t data_len)
{
    /* Pseudo-header (40 bytes) */
    uint8_t pseudo[40 + 4]; /* 40 = 2*16(src+dst) + 4(len) + 4(next+zero) */
    memcpy(pseudo, src, 16);
    memcpy(pseudo + 16, dst, 16);
    pseudo[32] = (data_len >> 24) & 0xFF;
    pseudo[33] = (data_len >> 16) & 0xFF;
    pseudo[34] = (data_len >> 8) & 0xFF;
    pseudo[35] = data_len & 0xFF;
    pseudo[36] = 0;
    pseudo[37] = 0;
    pseudo[38] = 0;
    pseudo[39] = next_hdr;

    /* Build composite buffer: pseudo-header + data */
    /* We compute checksum incrementally to avoid large stack allocation */
    uint32_t sum = 0;
    int count = 44; /* 40 + 4 zero padding = 44 bytes total for pseudo */
    const uint16_t *ptr = (const uint16_t *)pseudo;
    while (count > 1) {
        sum += *ptr++;
        count -= 2;
    }
    if (count)
        sum += *(const uint8_t *)ptr;

    ptr = (const uint16_t *)data;
    count = data_len;
    while (count > 1) {
        sum += *ptr++;
        count -= 2;
    }
    if (count)
        sum += *(const uint8_t *)ptr;

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

/* ── Send Ethernet frame with IPv6 ethertype ─────────────────────── */

void send_eth_ipv6(const uint8_t *dst_mac, const void *payload,
                    uint16_t len)
{
    send_eth(dst_mac, ETH_TYPE_IPV6, payload, len);
}

/* ── Send IPv6 packet ────────────────────────────────────────────── */

/* Compute the Ethernet destination MAC for a given IPv6 address:
 *  - For multicast addresses, map to IPv6 multicast MAC (33:33:XX:XX:XX:XX)
 *  - Otherwise look up in neighbor cache (or fall back to all-nodes) */
static void ipv6_resolve_dst_mac(const struct in6_addr *dst, uint8_t *mac_out)
{
    if (ipv6_addr_is_multicast(dst)) {
        /* IPv6 multicast MAC: 33:33:XX:XX:XX:XX (lower 32 bits of IPv6 addr) */
        mac_out[0] = 0x33;
        mac_out[1] = 0x33;
        mac_out[2] = dst->s6_addr[12];
        mac_out[3] = dst->s6_addr[13];
        mac_out[4] = dst->s6_addr[14];
        mac_out[5] = dst->s6_addr[15];
        return;
    }
    uint8_t *cached = ipv6_nd_cache_lookup(dst);
    if (cached) {
        memcpy(mac_out, cached, 6);
        return;
    }
    /* Fallback: send to all-nodes multicast MAC */
    mac_out[0] = 0x33;
    mac_out[1] = 0x33;
    mac_out[2] = 0x00;
    mac_out[3] = 0x00;
    mac_out[4] = 0x00;
    mac_out[5] = 0x01;
}

/*
 * ── IPv6 fragmentation on send (RFC 8200 §4.5) ──────────────────────
 *
 * IPv6 fragmentation is done ONLY by the source node (routers never
 * fragment).  When the payload exceeds the path MTU (typically 1460
 * bytes for Ethernet), we split it into fragments carrying Fragment
 * Headers.
 *
 * Each fragment's data length must be a multiple of 8 bytes for all
 * fragments except the last.  The Fragment Header is 8 bytes.
 */

/* Ethernet MTU for IPv6: 1500 - 40 (IPv6 header) = 1460 payload max */
#define IPV6_MAX_PAYLOAD  1460
/* Per-fragment data payload (after Fragment Header), rounded to 8 */
#define IPV6_FRAG_DATA    (uint16_t)(((IPV6_MAX_PAYLOAD - sizeof(struct ipv6_fragment)) / 8) * 8)

/* Counter for IPv6 fragment identification */
static uint32_t ipv6_frag_id_counter;

/* Build and send an IPv6 fragment.
 *
 * Parameters:
 *   dst          — destination IPv6 address
 *   src          — source IPv6 address
 *   next_hdr     — next header for this fragment (upper-layer protocol for
 *                  the first fragment, IPV6_NEXTHDR_FRAGMENT for subsequent)
 *   data         — fragment payload data (pointer into the original payload at offset)
 *   data_len     — length of fragment data
 *   frag_offset  — fragment offset in 8-octet units
 *   more         — More Fragments flag
 *   identification — fragment identification value
 */
static void send_ipv6_fragment(const struct in6_addr *dst,
                                const struct in6_addr *src,
                                uint8_t next_hdr,
                                const void *data, uint16_t data_len,
                                uint16_t frag_offset, int more,
                                uint32_t identification)
{
    uint8_t buf[IPV6_MAX_PAYLOAD + sizeof(struct ipv6_header)];
    struct ipv6_header *ip6 = (struct ipv6_header *)buf;
    struct ipv6_fragment *fh;
    uint16_t total;

    memset(buf, 0, sizeof(buf));

    /* Build IPv6 header */
    ip6->vcl_flow = htonl(0x60000000U);
    /* payload_length includes Fragment Header + fragment data */
    ip6->payload_length = htons((uint16_t)(sizeof(struct ipv6_fragment) + data_len));
    ip6->next_header = IPV6_NEXTHDR_FRAGMENT;  /* next is the Fragment Header */
    ip6->hop_limit = 64;
    memcpy(&ip6->src_ip, src, sizeof(struct in6_addr));
    memcpy(&ip6->dst_ip, dst, sizeof(struct in6_addr));

    /* Build Fragment Header */
    fh = (struct ipv6_fragment *)(buf + sizeof(struct ipv6_header));
    fh->next_header = next_hdr;  /* upper-layer protocol or next extension */
    fh->reserved = 0;
    fh->frag_off_more = htons((uint16_t)((frag_offset << 3) | (more ? 1 : 0)));
    fh->identification = htonl(identification);

    /* Copy fragment data */
    if (data_len > 0)
        memcpy(buf + sizeof(struct ipv6_header) + sizeof(struct ipv6_fragment),
               data, data_len);

    total = (uint16_t)(sizeof(struct ipv6_header) +
                       sizeof(struct ipv6_fragment) + data_len);

    /* Resolve destination MAC and send */
    {
        uint8_t dst_mac[6];
        ipv6_resolve_dst_mac(dst, dst_mac);
        send_eth_ipv6(dst_mac, buf, total);
    }
}

/* Send an IPv6 datagram as fragments.
 *
 * Splits the payload into fragments of IPV6_FRAG_DATA bytes each,
 * creates a unique identification value, and sends all fragments.
 * The last fragment has More Fragments flag = 0.
 */
void send_ipv6_fragmented(const struct in6_addr *dst, uint8_t next_hdr,
                           const void *payload, uint16_t len,
                           uint32_t identification, uint16_t effective_mtu)
{
	struct ipv6_addr_entry *src_entry;
	struct in6_addr src_addr;
	uint16_t off = 0;
	uint8_t frag_next_hdr;
	int frag_num = 0;

	/* Compute per-fragment data size based on effective MTU.
	 * Each fragment: IPv6 header (40) + Fragment Header (8) + data.
	 * The data size is rounded down to a multiple of 8 per RFC 8200. */
	uint16_t frag_data;
	if (effective_mtu >= sizeof(struct ipv6_header) +
	                       sizeof(struct ipv6_fragment) + 8) {
		frag_data = (uint16_t)(((effective_mtu -
			sizeof(struct ipv6_header) -
			sizeof(struct ipv6_fragment)) / 8) * 8);
	} else {
		frag_data = IPV6_FRAG_DATA; /* fallback */
	}

    /* Select source address */
    src_entry = ipv6_addr_select_source(dst);
    if (src_entry) {
        memcpy(&src_addr, &src_entry->addr, sizeof(struct in6_addr));
    } else {
        memcpy(&src_addr, &net_our_ipv6_ll, sizeof(struct in6_addr));
    }

	while (off < len) {
		uint16_t chunk = len - off;
		int more = 1;

		if (chunk > frag_data) {
			chunk = frag_data;
			more = 1;
		} else {
			more = 0;  /* last fragment */
		}

		/* First fragment carries the upper-layer protocol in next_header;
		 * subsequent fragments use IPV6_NEXTHDR_NONE (or the upper-layer
		 * protocol — RFC 8200 allows either but says the receiver should
		 * use the first fragment's next_header).  We follow the common
		 * practice: all fragments after the first use NONE. */
		frag_next_hdr = (off == 0) ? next_hdr : IPV6_NEXTHDR_NONE;
		(void)frag_num;

		send_ipv6_fragment(dst, &src_addr,
		                    frag_next_hdr,
		                    (const uint8_t *)payload + off, chunk,
		                    off / 8, more, identification);

		off = (uint16_t)(off + chunk);
		frag_num++;
	}
}

/* Send an atomic fragment: a packet with a Fragment Header but
 * offset=0 and M=0, meaning the packet is not actually fragmented.
 *
 * Atomic fragments are used by Path MTU Discovery (RFC 4821) when the
 * sender wants to include a Fragment Header with a non-zero
 * identification for loss-detection purposes, or by IPsec ESP when
 * the ESP encapsulation requires a Fragment Header to be present.
 */
void send_ipv6_atomic(const struct in6_addr *dst, uint8_t next_hdr,
                       const void *payload, uint16_t len,
                       uint32_t identification)
{
    struct ipv6_addr_entry *src_entry;
    struct in6_addr src_addr;

    /* Select source address */
    src_entry = ipv6_addr_select_source(dst);
    if (src_entry) {
        memcpy(&src_addr, &src_entry->addr, sizeof(struct in6_addr));
    } else {
        memcpy(&src_addr, &net_our_ipv6_ll, sizeof(struct in6_addr));
    }

    send_ipv6_fragment(dst, &src_addr, next_hdr,
                        payload, len, 0, 0, identification);
}

/* ── IPv6 flow label (RFC 6437) ──────────────────────────────────────
 *
 * The flow label is a 20-bit field in the IPv6 header used to identify
 * packets belonging to the same flow for QoS/classification.  Per RFC
 * 6437 §3, the flow label should be a pseudo-random hash of the
 * transport-layer 5-tuple (source address, destination address, source
 * port, destination port, protocol) combined with a secret seed.
 *
 * The hash uses XOR-folding of a simple 32-bit hash to produce a
 * uniform 20-bit value that is stable for the same 5-tuple but
 * unpredictable off-path.
 */

/* Secret seed for flow label computation — initialized once at boot */
static uint32_t flow_label_seed = 0;

/* Initialize the flow label seed from the kernel RNG */
void ipv6_flow_label_init(void)
{
    if (flow_label_seed == 0)
        flow_label_seed = (uint32_t)rng_get_u64();
}

/* Compute a 20-bit flow label from the transport-layer 5-tuple.
 *
 * Algorithm: 32-bit hash = Jenkins one-at-a-time over (src, dst, ports,
 * protocol, seed), then fold to 20 bits by XORing upper and lower halves
 * of the hash and masking to IPV6_FLOW_LABEL_MASK.
 *
 * Returns 0 when flow_label_seed is 0 (uninitialised) as a safety fallback.
 */
uint32_t ipv6_flow_label_calc(const struct in6_addr *src,
                               const struct in6_addr *dst,
                               uint16_t src_port, uint16_t dst_port,
                               uint8_t protocol)
{
    uint32_t hash;
    uint32_t h;
    int i;

    if (flow_label_seed == 0)
        return 0;

    /* Jenkins one-at-a-time hash over the 5-tuple + seed */
    h = 0;

    /* Source address (16 bytes) */
    for (i = 0; i < 16; i++) {
        h += src->s6_addr[i];
        h += (h << 10);
        h ^= (h >> 6);
    }

    /* Destination address (16 bytes) */
    for (i = 0; i < 16; i++) {
        h += dst->s6_addr[i];
        h += (h << 10);
        h ^= (h >> 6);
    }

    /* Source + destination ports (network byte order) */
    h += (uint32_t)(src_port >> 8) | ((uint32_t)(src_port & 0xFF) << 8);
    h += (h << 10);
    h ^= (h >> 6);

    h += (uint32_t)(dst_port >> 8) | ((uint32_t)(dst_port & 0xFF) << 8);
    h += (h << 10);
    h ^= (h >> 6);

    /* Protocol */
    h += protocol;
    h += (h << 10);
    h ^= (h >> 6);

    /* Secret seed */
    h += flow_label_seed;
    h += (h << 10);
    h ^= (h >> 6);

    /* Final avalanche */
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);

    /* Fold to 20 bits: XOR upper 12 bits with lower 20 bits */
    hash = (h & IPV6_FLOW_LABEL_MASK) ^ ((h >> 20) & 0x00000FFFU);

    /* Mask to exactly 20 bits (non-zero is preferred per RFC 6437 §6) */
    if (hash == 0)
        hash = 1;

    return hash & IPV6_FLOW_LABEL_MASK;
}

void send_ipv6_flow(const struct in6_addr *dst, uint8_t next_hdr,
                    const void *payload, uint16_t len,
                    uint32_t flow_label)
{
    uint8_t buf[2048];
    struct ipv6_header *ip6 = (struct ipv6_header *)buf;
    struct ipv6_addr_entry *src_entry;

    /* Determine effective MTU for this destination using PMTU cache */
    uint16_t link_payload_mtu = (net_ipv6_link_mtu > (uint32_t)sizeof(struct ipv6_header))
        ? (uint16_t)(net_ipv6_link_mtu - sizeof(struct ipv6_header))
        : IPV6_MIN_MTU;
    uint16_t effective_mtu = link_payload_mtu;
    uint16_t cached_pmtu = ipv6_pmtu_lookup(dst);
    if (cached_pmtu > 0 && cached_pmtu < effective_mtu)
        effective_mtu = cached_pmtu;

    /* Fragment if payload exceeds the effective path MTU */
    if (len > effective_mtu) {
        uint32_t id = __sync_fetch_and_add(&ipv6_frag_id_counter, 1);
        kprintf("[ipv6] fragmenting: payload %u bytes > mtu %u, id=%u\n",
                len, effective_mtu, id);
        send_ipv6_fragmented(dst, next_hdr, payload, len, id, effective_mtu);
        return;
    }

    /* Build IPv6 header */
    /* Version=6, Traffic Class=0, Flow Label=flow_label */
    ip6->vcl_flow = htonl(0x60000000U | (flow_label & IPV6_FLOW_LABEL_MASK));
    ip6->payload_length = htons(len);
    ip6->next_header = next_hdr;
    ip6->hop_limit = 64;  /* default hop limit */

    /* Select source address based on destination */
    src_entry = ipv6_addr_select_source(dst);
    if (src_entry) {
        memcpy(&ip6->src_ip, &src_entry->addr, sizeof(struct in6_addr));
    } else {
        /* Fallback: use link-local */
        memcpy(&ip6->src_ip, &net_our_ipv6_ll, sizeof(struct in6_addr));
    }
    memcpy(&ip6->dst_ip, dst, sizeof(struct in6_addr));

    /* Copy payload after header */
    if (len > 0)
        memcpy(buf + sizeof(struct ipv6_header), payload, len);

    uint16_t total = sizeof(struct ipv6_header) + len;

    /* Resolve destination MAC and send */
    {
        uint8_t dst_mac[6];
        ipv6_resolve_dst_mac(dst, dst_mac);
        send_eth_ipv6(dst_mac, buf, total);
    }
}

/* send_ipv6 — convenience wrapper with no flow label */
void send_ipv6(const struct in6_addr *dst, uint8_t next_hdr,
               const void *payload, uint16_t len)
{
    send_ipv6_flow(dst, next_hdr, payload, len, 0);
}

/* ── ICMPv6 Echo (ping6) ─────────────────────────────────────────── */

static int ping6_reply_received = 0;

/*
 * Handle incoming ICMPv6 Echo Request (type 128).
 *
 * Per RFC 4443 §4.1, an Echo Reply must:
 *  - Set Type = 129 (Echo Reply)
 *  - Set Code = 0
 *  - Copy the Identifier and Sequence Number from the request
 *  - Compute the ICMPv6 Checksum using the correct source address
 *    (the address the request was sent to)
 *  - Include any data that was in the request body
 */
static void handle_icmpv6_echo_request(struct ipv6_header *ip6,
                                        const uint8_t *payload,
                                        uint16_t len)
{
    struct ipv6_addr_entry *src_entry;
    struct in6_addr src_addr;

    /* Validate minimum length: must have at least type+code+checksum+id+seq */
    if (len < sizeof(struct icmpv6_echo))
        return;

    /* Code must be 0 for Echo Request (RFC 4443 §4.1) */
    if (payload[1] != 0)
        return;

    /* Verify ICMPv6 checksum of incoming request before responding */
    {
        const struct icmpv6_header *req_hdr =
            (const struct icmpv6_header *)payload;
        uint16_t recv_csum = req_hdr->checksum;
        uint16_t calc_csum = ipv6_checksum(&ip6->src_ip, &ip6->dst_ip,
                                            IP_PROTO_ICMPV6,
                                            payload, len);
        if (recv_csum != calc_csum)
            return;
    }

    /* Prepare reply buffer — limit to stack-allocated size */
    uint8_t reply_buf[1500];
    uint16_t reply_len = len < sizeof(reply_buf) ? len : sizeof(reply_buf);
    memcpy(reply_buf, payload, reply_len);

    struct icmpv6_header *icmp = (struct icmpv6_header *)reply_buf;
    icmp->type = 129; /* Echo Reply */
    icmp->code = 0;

    /*
     * Select the source address that send_ipv6() will use for the
     * reply.  This ensures the ICMPv6 checksum is computed over the
     * same source address that appears in the IPv6 header.
     */
    src_entry = ipv6_addr_select_source(&ip6->src_ip);
    if (src_entry) {
        memcpy(&src_addr, &src_entry->addr, sizeof(struct in6_addr));
    } else {
        memcpy(&src_addr, &net_our_ipv6_ll, sizeof(struct in6_addr));
    }

    icmp->checksum = 0;
    icmp->checksum = ipv6_checksum(&src_addr, &ip6->src_ip,
                                    IP_PROTO_ICMPV6,
                                    reply_buf, reply_len);

    send_ipv6(&ip6->src_ip, IP_PROTO_ICMPV6, reply_buf, reply_len);
}

/*
 * Handle incoming ICMPv6 Echo Reply (type 129).
 *
 * Only accepts replies that match our expected identifier, preventing
 * stray ICMPv6 packets from falsely completing a ping6() call.
 */
static void handle_icmpv6_echo_reply(const uint8_t *payload, uint16_t len)
{
    /* Must have at least the echo header with id and seq */
    if (len < sizeof(struct icmpv6_echo))
        return;

    const struct icmpv6_echo *reply =
        (const struct icmpv6_echo *)payload;

    /* Match against the identifier used by ipv6_ping6() */
    if (reply->id == (uint16_t)htons(0x1234))
        ping6_reply_received = 1;
}

/* ── NDP: Handled by ipv6_ndisc.c ────────────────────────────────── */

void ipv6_send_rs(void)
{
    ipv6_nd_send_rs();
}

/* ── SLAAC: RA processing moved to ipv6_ndisc.c ──────────────────── */

/* ── ICMPv6 dispatcher ───────────────────────────────────────────── */

void handle_icmpv6(struct ipv6_header *ip6, const uint8_t *payload,
                    uint16_t len)
{
	if (len < sizeof(struct icmpv6_header)) return;

	const struct icmpv6_header *icmp = (const struct icmpv6_header *)payload;

	switch (icmp->type) {
	case 128: /* Echo Request */
		handle_icmpv6_echo_request(ip6, payload, len);
		break;
	case 129: /* Echo Reply */
		handle_icmpv6_echo_reply(payload, len);
		break;
	case ICMPV6_PACKET_TOO_BIG: /* 2 — Packet Too Big (RFC 4443 §3.2) */
	{
		/*
		 * ICMPv6 Packet Too Big body:
		 *   struct icmpv6_header (4 bytes)
		 *   uint32_t mtu          (4 bytes)
		 *   uint8_t  offending[]  (portion of original packet)
		 *
		 * We extract the MTU and update the PMTU cache for the
		 * original packet's destination.
		 */
		if (len < sizeof(struct icmpv6_header) + sizeof(uint32_t))
			break;

		const uint32_t *mtu_field = (const uint32_t *)(payload +
			sizeof(struct icmpv6_header));
		uint32_t mtu = ntohl(*mtu_field);

		/* Clamp to valid IPv6 MTU range */
		if (mtu < IPV6_MIN_MTU)
			mtu = IPV6_MIN_MTU;
		if (mtu > IPV6_DEFAULT_LINK_MTU)
			mtu = IPV6_DEFAULT_LINK_MTU;

		kprintf("[ipv6] ICMPv6 Packet Too Big: MTU=%u\n", mtu);

		/*
		 * Update PMTU cache for the destination of the offending
		 * packet.  The offending IPv6 header is embedded in the
		 * ICMPv6 payload after the MTU field.
		 */
		uint16_t data_off = (uint16_t)(sizeof(struct icmpv6_header) +
		                                sizeof(uint32_t));
		if (len >= data_off + (uint16_t)sizeof(struct ipv6_header)) {
			const struct ipv6_header *offending =
				(const struct ipv6_header *)(payload + data_off);
			ipv6_pmtu_update(&offending->dst_ip, (uint16_t)mtu);
		}
		break;
	}
	case ICMPV6_NS: /* Neighbor Solicitation */
		ipv6_nd_handle_ns(ip6, payload, len);
		break;
	case ICMPV6_NA: /* Neighbor Advertisement */
		ipv6_nd_handle_na(ip6, payload, len);
		break;
	case ICMPV6_RS: /* Router Solicitation */
		ipv6_nd_handle_rs(ip6, payload, len);
		break;
	case ICMPV6_RA: /* Router Advertisement */
		ipv6_nd_handle_ra(ip6, payload, len);
		break;
	case ICMPV6_MLD_QUERY: /* Multicast Listener Query */
		ipv6_mld_handle_query(ip6, payload, len);
		break;
	case ICMPV6_MLD_REPORT: /* MLDv1 Report */
		ipv6_mld_handle_report_v1(ip6, payload, len);
		break;
	case ICMPV6_MLD_REPORT_V2: /* MLDv2 Report */
		ipv6_mld_handle_report_v2(ip6, payload, len);
		break;
	default:
		break;
	}
}

/* ── IPv6 dispatcher ─────────────────────────────────────────────── */

void handle_ipv6(const uint8_t *data, uint16_t len)
{
    /* Delegate to the full extension-header-aware handler in ipv6_core.c */
    handle_ipv6_packet(data, len);
}

/* ── ping6 ────────────────────────────────────────────────────────── */

int ipv6_ping6(const struct in6_addr *target)
{
    struct ipv6_addr_entry *src_entry;
    struct in6_addr src_addr;

    if (!net_ipv6_ll_ready) return -1;

    /* Pre-select the source address that send_ipv6() will use,
     * so the ICMPv6 checksum is computed over the same source
     * address that appears in the IPv6 header. */
    src_entry = ipv6_addr_select_source(target);
    if (src_entry) {
        memcpy(&src_addr, &src_entry->addr, sizeof(struct in6_addr));
    } else {
        memcpy(&src_addr, &net_our_ipv6_ll, sizeof(struct in6_addr));
    }

    uint8_t buf[128];
    struct icmpv6_echo *echo = (struct icmpv6_echo *)buf;
    echo->hdr.type = 128; /* Echo Request */
    echo->hdr.code = 0;
    echo->id   = htons(0x1234);

    uint16_t data_start = sizeof(struct icmpv6_echo);

    for (int seq = 1; seq <= 4; seq++) {
        echo->seq = htons((uint16_t)seq);

        /* Fill payload with pattern data */
        for (int i = 0; i < 32; i++)
            buf[data_start + i] = (uint8_t)i;

        uint16_t pkt_len = data_start + 32;

        echo->hdr.checksum = 0;
        echo->hdr.checksum = ipv6_checksum(&src_addr, target,
                                        IP_PROTO_ICMPV6, buf, pkt_len);

        ping6_reply_received = 0;
        send_ipv6(target, IP_PROTO_ICMPV6, buf, pkt_len);

        uint64_t start = timer_get_ticks();
        while (!ping6_reply_received) {
            net_poll();
            uint64_t now = timer_get_ticks();
            if (now - start > 200) break; /* 2 second timeout */
        }
        if (ping6_reply_received) {
            uint64_t elapsed = timer_get_ticks() - start;
            return (int)(elapsed * 10);
        }
    }
    return -1;
}

/* ── SLAAC address lifetime management (RFC 4862 §5.5.4) ──────────── */

/* Poll SLAAC address lifetimes.
 *
 * Called from ipv6_poll() once per timer tick.  Manages:
 *  - PREFERRED → DEPRECATED transition when preferred_lifetime expires
 *  - Address removal when valid_lifetime expires (dereferencing
 *    net_our_ipv6_gua / net_ipv6_gua_valid)
 *  - Cleanup of the global GUA pointers when the SLAAC address dies
 *
 * Per RFC 4862 §5.5.4:
 *   An address transitions from PREFERRED to DEPRECATED when its
 *   preferred_lifetime expires.  A deprecated address SHOULD NOT
 *   be used as a source for new communications, but existing
 *   connections may continue (the state allows outbound but
 *   deprioritises it via ipv6_addr_select_source).
 *
 *   When the valid_lifetime expires the address MUST be removed
 *   from the interface entirely.
 */
static void ipv6_slaac_poll(void)
{
	uint64_t now = timer_get_ticks();
	int i;

	for (i = 0; i < IPV6_ADDR_TABLE_SIZE; i++) {
		struct ipv6_addr_entry *e = &ipv6_addr_table[i];

		if (!e->valid)
			continue;

		/* Only manage SLAAC-autoconfigured addresses */
		if (!(e->flags & IPV6_ADDR_F_AUTOCONF))
			continue;

		/* ── preferred_lifetime expired → DEPRECATED ────────────── */
		if (e->state == IPV6_ADDR_STATE_PREFERRED &&
		    e->preferred_expiry_tick != UINT64_MAX &&
		    now >= e->preferred_expiry_tick) {
			e->state = IPV6_ADDR_STATE_DEPRECATED;
			kprintf("[slaac] address %02x%02x:... "
			        "DEPRECATED (preferred lifetime expired)\n",
			        e->addr.s6_addr[0], e->addr.s6_addr[1]);
		}

		/* ── valid_lifetime expired → remove ──────────────────── */
		if (e->expiry_tick != UINT64_MAX &&
		    now >= e->expiry_tick) {
			/* If this was the tracked GUA, clear the pointer */
			if (net_ipv6_gua_valid &&
			    ipv6_addr_equal(&e->addr, &net_our_ipv6_gua)) {
				net_ipv6_gua_valid = 0;
				memset(&net_our_ipv6_gua, 0,
				       sizeof(struct in6_addr));
				kprintf("[slaac] GUA removed (valid lifetime "
				        "expired)\n");
			}

			kprintf("[slaac] address %02x%02x:... "
			        "removed (valid lifetime expired)\n",
			        e->addr.s6_addr[0], e->addr.s6_addr[1]);
			memset(e, 0, sizeof(*e));
			ipv6_addr_count--;
		}
	}
}

/* ── Initialization ──────────────────────────────────────────────── */

void ipv6_init(void)
{
	/* Generate link-local address from MAC */
	struct in6_addr ll_prefix = IPV6_ADDR_LINKLOCAL_PFX;
	ipv6_eui64_from_mac(net_our_mac, &net_our_ipv6_ll);
	/* Set FE80:: prefix (first 10 bits) */
	memcpy(net_our_ipv6_ll.s6_addr, ll_prefix.s6_addr, 8);

	net_ipv6_ll_ready = 1;

	/* Initialise the NDISC module */
	ipv6_nd_init();

	/* Initialise the PMTU Discovery module */
	ipv6_pmtu_init();

	/* Register link-local address as TENTATIVE in the management table */
	/* DAD will confirm it and transition to PERMANENT */
	ipv6_addr_add(&net_our_ipv6_ll, 64, IPV6_ADDR_STATE_TENTATIVE,
	              0xFFFFFFFF, 0xFFFFFFFF, 0);
	/* Add all-nodes multicast address as a permanent entry */
	{
		struct in6_addr all_nodes = IPV6_ADDR_ALL_NODES;
		ipv6_addr_add(&all_nodes, 128, IPV6_ADDR_STATE_PERMANENT,
		              0xFFFFFFFF, 0xFFFFFFFF, 0);
	}

	kprintf("[IPv6] Link-local address configured, starting DAD\n");

	/* Initialise MLDv2 multicast group management */
	ipv6_mld_init();

	/* Start DAD for link-local address */
	ipv6_dad_start(&net_our_ipv6_ll);

	/* Send Router Solicitation to trigger SLAAC */
	rs_sent = 1;
	rs_retries = 0;
	rs_last_tick = timer_get_ticks();
	ipv6_send_rs();
}

/* ── Periodic poll tasks ─────────────────────────────────────────── */

void ipv6_poll(void)
{
	/* Run SLAAC address lifetime management */
	ipv6_slaac_poll();

	/* Run DAD (Duplicate Address Detection) state machine */
	ipv6_dad_poll();

	/* Run NDISC reachability state machine */
	ipv6_nd_poll();

	/* Run MLDv2 multicast listener timers */
	ipv6_mld_poll();

	/* Run PMTU cache expiry */
	ipv6_pmtu_poll();

	/* Retry RS if we haven't received an RA yet */
	if (rs_sent && !net_ipv6_gua_valid && rs_retries < RS_MAX_RETRIES) {
		uint64_t now = timer_get_ticks();
		if (now - rs_last_tick >= RS_RETRY_INTERVAL) {
			rs_retries++;
			rs_last_tick = now;
			ipv6_send_rs();
		}
	}
}

/* ── Public API ──────────────────────────────────────────────────── */

int ipv6_has_linklocal(void)
{
    return net_ipv6_ll_ready;
}

void ipv6_get_linklocal(struct in6_addr *addr)
{
    if (addr) memcpy(addr, &net_our_ipv6_ll, sizeof(struct in6_addr));
}

/* ── Implement: ipv6_send ────────────────── */
int ipv6_send(void *skb)
{
    if (!skb) {
        kprintf("[ipv6] ipv6_send: NULL skb\n");
        return -EINVAL;
    }
    kprintf("[ipv6] ipv6_send: skb=%p (stub)\n", skb);
    return -EOPNOTSUPP;
}
/* ── Implement: ipv6_route_add ────────────────── */
int ipv6_route_add(const void *dst, const void *gw, int ifindex)
{
    if (!dst || !gw) {
        kprintf("[ipv6] ipv6_route_add: NULL parameter\n");
        return -EINVAL;
    }
    if (ifindex < 0) {
        kprintf("[ipv6] ipv6_route_add: invalid ifindex %d\n", ifindex);
        return -EINVAL;
    }
    kprintf("[ipv6] ipv6_route_add: dst=%p gw=%p ifindex=%d (stub)\n", dst, gw, ifindex);
    return -EOPNOTSUPP;
}
/* ── Implement: ipv6_route_del ────────────────── */
int ipv6_route_del(const void *dst)
{
    if (!dst) {
        kprintf("[ipv6] ipv6_route_del: NULL dst\n");
        return -EINVAL;
    }
    kprintf("[ipv6] ipv6_route_del: dst=%p (stub)\n", dst);
    return -EOPNOTSUPP;
}
