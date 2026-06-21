/* net.c — Core networking: state, ethernet/IP, ARP, ICMP, poll, init */

#include "net_internal.h"
#include "e1000.h"
#include "virtio_net.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "waitqueue.h"
#include "netfilter.h"
#include "conntrack_helper.h"
#include "export.h"
#include "netdevice.h"
#include "net_rps.h"
#include "xdp.h"           /* XDP hook for early packet processing */

/* Network state lock — protects all of the following globals */
static spinlock_t net_lock = SPINLOCK_INIT;

/* Shared network state */
uint8_t  net_our_mac[6];
uint32_t net_our_ip;
uint32_t net_gateway_ip;
uint32_t net_subnet_mask;
int      net_dhcp_done = 0;
uint32_t net_dns_server = 0;
uint8_t  net_gw_mac[6];
int      net_gw_mac_known = 0;
uint16_t net_ip_id_counter = 1;

/* IP routing table */
struct rt_entry rt_table[RT_MAX_ENTRIES];
int rt_num_entries = 0;

/* IP forwarding */
int net_ip_forwarding = 0;

/* Network interface statistics */
struct net_iface_stats net_iface_stats;

/* ARP cache */
struct arp_entry net_arp_cache[ARP_CACHE_SIZE];

/* Pending ARP resolution queue — packets awaiting MAC resolution */
struct arp_pending_pkt arp_pending_queue[ARP_PENDING_QUEUE_SIZE];

/* TCP connection table */
struct tcp_conn tcp_conns[MAX_TCP_CONNS];

/* Listeners */
struct tcp_listener net_listeners[MAX_LISTENERS];
int net_num_listeners = 0;

/* UDP bindings */
struct udp_binding net_udp_bindings[MAX_UDP_BINDINGS];
int net_num_udp_bindings = 0;

/* Packet receive buffer */
static uint8_t pkt_buf[2048];
static volatile int net_rx_flag = 0;
static struct wait_queue net_rx_wq;

void net_rx_signal(void) {
    net_rx_flag = 1;
    wait_queue_wake(&net_rx_wq);
}

int  net_rx_pending(void) { return net_rx_flag; }

int net_link_recv(void *buf, uint16_t max_len) {
    if (virtio_net_present()) {
        int n = virtio_net_receive(buf, max_len);
        if (n != 0) return n;
    }
    if (e1000_is_present())
        return e1000_receive(buf, max_len);
    return 0;
}

int net_link_send(const void *data, uint16_t len) {
    /* Prefer the netdevice layer if interfaces are registered.
     * Find the interface whose MAC matches our MAC (the primary NIC). */
    if (netif_count() > 0) {
        for (int i = 0; i < NETDEV_MAX; i++) {
            struct net_device *dev = netif_get(i);
            if (dev && dev->transmit &&
                memcmp(dev->mac, net_our_mac, 6) == 0) {
                kprintf("[dbg] net_link_send: mac match at %d, calling transmit\n", i);
                int ret = dev->transmit(dev, (const uint8_t *)data, len);
                kprintf("[dbg] net_link_send: transmit returned %d\n", ret);
                return ret;
            }
        }
        kprintf("[dbg] net_link_send: no mac match (netif_count=%d)\n", netif_count());
    }
    /* Fallback: direct driver calls for legacy compatibility */
    if (virtio_net_present()) {
        kprintf("[dbg] net_link_send: fallback virtio\n");
        if (virtio_net_send((const uint8_t *)data, len) < 0)
            return -1;
        return 0;
    }
    kprintf("[dbg] net_link_send: fallback e1000_send\n");
    return e1000_send(data, len);
}

/* ICMP ping state */
static volatile int ping_reply_received = 0;

/* ── IP routing table ───────────────────────────────────────────── */

int rt_add(uint32_t dst, uint32_t mask, uint32_t gw, int iface) {
    spinlock_acquire(&net_lock);
    if (rt_num_entries >= RT_MAX_ENTRIES) {
        spinlock_release(&net_lock);
        return -1;
    }
    /* Check for duplicate */
    for (int i = 0; i < rt_num_entries; i++) {
        if (rt_table[i].dst == dst && rt_table[i].mask == mask)
            return -1;
    }
    rt_table[rt_num_entries].dst   = dst;
    rt_table[rt_num_entries].mask  = mask;
    rt_table[rt_num_entries].gw    = gw;
    rt_table[rt_num_entries].iface = iface;
    rt_num_entries++;
    spinlock_release(&net_lock);
    return 0;
}

int rt_del(uint32_t dst, uint32_t mask) {
    spinlock_acquire(&net_lock);
    for (int i = 0; i < rt_num_entries; i++) {
        if (rt_table[i].dst == dst && rt_table[i].mask == mask) {
            for (int j = i; j < rt_num_entries - 1; j++)
                rt_table[j] = rt_table[j + 1];
            rt_num_entries--;
            spinlock_release(&net_lock);
            return 0;
        }
    }
    spinlock_release(&net_lock);
    return -1;
}

int rt_lookup(uint32_t ip, uint32_t *gw_out, int *iface_out) {
    int best = -1;
    uint32_t best_mask = 0;
    spinlock_acquire(&net_lock);
    for (int i = 0; i < rt_num_entries; i++) {
        if ((ip & rt_table[i].mask) == (rt_table[i].dst & rt_table[i].mask)) {
            if (rt_table[i].mask > best_mask) {
                best = i;
                best_mask = rt_table[i].mask;
            }
        }
    }
    if (best < 0) { spinlock_release(&net_lock); return -1; }
    if (gw_out)   *gw_out   = rt_table[best].gw;
    if (iface_out) *iface_out = rt_table[best].iface;
    spinlock_release(&net_lock);
    return 0;
}

void rt_clear(void) {
    spinlock_acquire(&net_lock);
    rt_num_entries = 0;
    spinlock_release(&net_lock);
}

/* ── rt_flush — flush routing table (alias for tests) ────────────── */
void rt_flush(void) {
    rt_clear();
}

/* ── Gratuitous ARP ────────────────────────────────────────────── */

void arp_announce(void) {
    if (!net_our_ip) return;
    struct arp_packet arp;
    arp.hw_type = htons(1);
    arp.proto_type = htons(ETH_TYPE_IP);
    arp.hw_len = 6;
    arp.proto_len = 4;
    arp.opcode = htons(2); /* ARP reply */
    memcpy(arp.sender_mac, net_our_mac, 6);
    arp.sender_ip = htonl(net_our_ip);
    memcpy(arp.target_mac, net_our_mac, 6);
    arp.target_ip = htonl(net_our_ip);
    uint8_t bc[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    send_eth(bc, ETH_TYPE_ARP, &arp, sizeof(arp));
}

/* ── Interface stats tracking ──────────────────────────────────── */
/* Call these from link-level send/recv after successful operations.
   net.c's send_eth and net_poll already call net_link_send/net_link_recv,
   so we hook into those. */

/* --- ARP cache --- */

void arp_cache_add(uint32_t ip, const uint8_t *mac) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (net_arp_cache[i].valid && net_arp_cache[i].ip == ip) {
            /* Rate-limit ARP updates to 10/sec to prevent cache poisoning */
            if (now - net_arp_cache[i].last_seen_tick < (TIMER_FREQ / 10))
                return;
            memcpy(net_arp_cache[i].mac, mac, 6);
            net_arp_cache[i].last_seen_tick = now;
            net_arp_cache[i].retry_count = 0;
            net_arp_cache[i].resolving = 0;
            /* Flush any packets queued for this IP */
            arp_flush_pending(ip);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!net_arp_cache[i].valid) {
            net_arp_cache[i].ip = ip;
            memcpy(net_arp_cache[i].mac, mac, 6);
            net_arp_cache[i].valid = 1;
            net_arp_cache[i].last_seen_tick = now;
            net_arp_cache[i].retry_count = 0;
            net_arp_cache[i].resolving = 0;
            net_arp_cache[i].last_probe_tick = 0;
            /* Flush any packets queued for this IP */
            arp_flush_pending(ip);
            return;
        }
    }
    /* Evict oldest entry */
    int oldest = 0;
    uint64_t oldest_tick = net_arp_cache[0].last_seen_tick;
    for (int i = 1; i < ARP_CACHE_SIZE; i++) {
        if (net_arp_cache[i].last_seen_tick < oldest_tick) {
            oldest = i;
            oldest_tick = net_arp_cache[i].last_seen_tick;
        }
    }
    net_arp_cache[oldest].ip = ip;
    memcpy(net_arp_cache[oldest].mac, mac, 6);
    net_arp_cache[oldest].valid = 1;
    net_arp_cache[oldest].last_seen_tick = now;
    net_arp_cache[oldest].retry_count = 0;
    net_arp_cache[oldest].resolving = 0;
    net_arp_cache[oldest].last_probe_tick = 0;
    arp_flush_pending(ip);
}

uint8_t *arp_cache_lookup(uint32_t ip) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (net_arp_cache[i].valid && net_arp_cache[i].ip == ip) {
            /* Check if entry has expired */
            if (now - net_arp_cache[i].last_seen_tick > ARP_TIMEOUT_TICKS) {
                /* Mark stale — still return mac but GC will clean up */
                /* We don't clear valid here; arp_gc() handles expiry */
            }
            return net_arp_cache[i].mac;
        }
    }
    return NULL;
}

/* Send an ARP request for the given IP */
void arp_send_request(uint32_t target_ip) {
    struct arp_packet arp;
    arp.hw_type = htons(1);
    arp.proto_type = htons(ETH_TYPE_IP);
    arp.hw_len = 6;
    arp.proto_len = 4;
    arp.opcode = htons(1);
    memcpy(arp.sender_mac, net_our_mac, 6);
    arp.sender_ip = htonl(net_our_ip);
    memset(arp.target_mac, 0, 6);
    arp.target_ip = htonl(target_ip);
    uint8_t bc[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    send_eth(bc, ETH_TYPE_ARP, &arp, sizeof(arp));
}

/* Resolve gateway MAC via ARP with retries + polling */
void arp_resolve_gateway(void) {
    if (net_gw_mac_known || !net_gateway_ip) return;
    for (int a = 0; a < 3 && !net_gw_mac_known; a++) {
        arp_send_request(net_gateway_ip);
        uint64_t start = timer_get_ticks();
        volatile uint32_t w = 0;
        while (!net_gw_mac_known) {
            net_poll();
            w++;
            uint64_t now = timer_get_ticks();
            /* Tick-based timeout (2s) when timer is running, else spin-count */
            if (now > start && now - start > 200) break;
            if (now == start && w > 3000000) break;
        }
    }
}

/* Resolve an arbitrary local IP via ARP with retries + polling */
static void arp_resolve_ip(uint32_t ip) {
    if (arp_cache_lookup(ip)) return;
    for (int a = 0; a < 3 && !arp_cache_lookup(ip); a++) {
        arp_send_request(ip);
        uint64_t start = timer_get_ticks();
        volatile uint32_t w = 0;
        while (!arp_cache_lookup(ip)) {
            net_poll();
            w++;
            uint64_t now = timer_get_ticks();
            if (now > start && now - start > 200) break;
            if (now == start && w > 3000000) break;
        }
    }
}

/* ── ARP garbage collector — expire stale entries ─────────────────
 *
 * Called from net_poll() to clean up entries that haven't been
 * refreshed within ARP_TIMEOUT_TICKS (5 min).  Stale entries are
 * marked invalid so subsequent lookups will trigger fresh resolution.
 */
void arp_gc(void)
{
    uint64_t now = timer_get_ticks();

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!net_arp_cache[i].valid)
            continue;

        uint64_t age = now - net_arp_cache[i].last_seen_tick;

        /* Hard expiry: entry hasn't been seen in too long */
        if (age > ARP_TIMEOUT_TICKS) {
            net_arp_cache[i].valid = 0;
            net_arp_cache[i].resolving = 0;
            net_arp_cache[i].retry_count = 0;
            continue;
        }

        /* If this entry is still marked as resolving but hasn't had
         * activity recently, reset the flag so another resolver can try. */
        if (net_arp_cache[i].resolving &&
            age > (uint64_t)ARP_RETRY_INTERVAL_TICKS * (ARP_MAX_RETRIES + 2)) {
            net_arp_cache[i].resolving = 0;
            net_arp_cache[i].retry_count = 0;
        }
    }

    /* Also expire any stale pending queue entries (stuck for > 30s) */
    for (int i = 0; i < ARP_PENDING_QUEUE_SIZE; i++) {
        if (arp_pending_queue[i].in_use) {
            uint64_t age = now - arp_pending_queue[i].enqueue_tick;
            if (age > (uint64_t)ARP_TIMEOUT_TICKS / 10) { /* 30 seconds */
                arp_pending_queue[i].in_use = 0;
            }
        }
    }
}

/* ── Retry pending ARP resolutions ────────────────────────────────
 *
 * Scans the ARP cache for entries that are marked "resolving" and
 * have exceeded the retry interval.  Sends a fresh ARP probe and
 * increments the retry count.  Entries exceeding ARP_MAX_RETRIES
 * are marked invalid so callers get a fresh entry on next lookup.
 */
void arp_retry_pending(void)
{
    uint64_t now = timer_get_ticks();

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!net_arp_cache[i].valid || !net_arp_cache[i].resolving)
            continue;

        uint64_t since_probe = now - net_arp_cache[i].last_probe_tick;

        if (since_probe < ARP_RETRY_INTERVAL_TICKS)
            continue;

        /* Time to send another probe */
        net_arp_cache[i].retry_count++;
        net_arp_cache[i].last_probe_tick = now;

        if (net_arp_cache[i].retry_count > ARP_MAX_RETRIES) {
            /* Resolution failed — invalidate entry so callers start fresh */
            net_arp_cache[i].valid = 0;
            net_arp_cache[i].resolving = 0;
            continue;
        }

        arp_send_request(net_arp_cache[i].ip);
    }
}

/* ── Flush queued packets for a now-resolved IP ───────────────────
 *
 * When an ARP reply arrives, any packets buffered in the pending
 * queue for that IP are sent out using the now-known MAC address.
 */
void arp_flush_pending(uint32_t ip)
{
    uint8_t *mac = arp_cache_lookup(ip);
    if (!mac) return;  /* Not yet resolved — don't flush */

    for (int i = 0; i < ARP_PENDING_QUEUE_SIZE; i++) {
        if (!arp_pending_queue[i].in_use)
            continue;
        if (arp_pending_queue[i].target_ip != ip)
            continue;

        /* Send the buffered frame using the resolved MAC.
         * The frame was stored as a complete Ethernet frame with the
         * destination MAC at offset 0; we update it to the resolved MAC. */
        memcpy(arp_pending_queue[i].data, mac, 6);
        net_link_send(arp_pending_queue[i].data, arp_pending_queue[i].len);

        arp_pending_queue[i].in_use = 0;
    }
}

/* ── Count pending ARP resolutions ──────────────────────────────── */
int arp_pending_count(void)
{
    int count = 0;
    for (int i = 0; i < ARP_PENDING_QUEUE_SIZE; i++) {
        if (arp_pending_queue[i].in_use)
            count++;
    }
    return count;
}

/* ── Resolve a target IP, queueing a frame if unresolved ──────────
 *
 * This is the main entry point for sending IP packets to a local
 * destination.  If the MAC is known, it returns 1 (caller can send
 * immediately).  If unknown, it sends an ARP request, buffers the
 * frame in the pending queue, and returns 0.  On queue overflow,
 * returns -1.
 *
 * The caller should retry sending after net_poll() has been called
 * enough times for the ARP reply to arrive.
 */
int arp_resolve_or_queue(uint32_t dst_ip,
                          const void *frame, uint16_t frame_len)
{
    uint8_t *mac = arp_cache_lookup(dst_ip);
    if (mac) {
        /* MAC is known — caller can send */
        return 1;
    }

    /* MAC unknown — start or continue resolution */
    uint64_t now = timer_get_ticks();

    /* Check if there's already an active resolution for this IP */
    int already_resolving = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (net_arp_cache[i].valid &&
            net_arp_cache[i].ip == dst_ip &&
            net_arp_cache[i].resolving) {
            already_resolving = 1;
            break;
        }
    }

    if (!already_resolving) {
        /* Start a fresh resolution */
        /* Allocate a temporary cache slot for the resolution state */
        int slot = -1;
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (!net_arp_cache[i].valid) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            /* Cache full — evict the oldest entry */
            uint64_t oldest_tick = net_arp_cache[0].last_seen_tick;
            slot = 0;
            for (int i = 1; i < ARP_CACHE_SIZE; i++) {
                if (net_arp_cache[i].last_seen_tick < oldest_tick) {
                    oldest_tick = net_arp_cache[i].last_seen_tick;
                    slot = i;
                }
            }
        }
        net_arp_cache[slot].ip = dst_ip;
        net_arp_cache[slot].valid = 1;
        net_arp_cache[slot].last_seen_tick = now;
        net_arp_cache[slot].resolving = 1;
        net_arp_cache[slot].retry_count = 0;
        net_arp_cache[slot].last_probe_tick = now;
        memset(net_arp_cache[slot].mac, 0, 6);

        /* Send the first ARP probe */
        arp_send_request(dst_ip);
    }

    /* Queue the frame for later delivery */
    if (frame_len > ARP_PENDING_MAX_PKT)
        return -1;  /* Frame too large to buffer */

    for (int i = 0; i < ARP_PENDING_QUEUE_SIZE; i++) {
        if (!arp_pending_queue[i].in_use) {
            arp_pending_queue[i].target_ip = dst_ip;
            memcpy(arp_pending_queue[i].data, frame, frame_len);
            arp_pending_queue[i].len = frame_len;
            arp_pending_queue[i].in_use = 1;
            arp_pending_queue[i].enqueue_tick = now;
            return 0;  /* Queued */
        }
    }

    return -1;  /* Queue full */
}

/* --- Checksum --- */

uint16_t net_checksum(const void *data, int len) {
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len == 1) sum += *(const uint8_t *)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~(uint16_t)sum;
}

/* --- IP getters/setters --- */

void net_get_ip(uint8_t *ip) {
    ip[0] = (uint8_t)((net_our_ip >> 24) & 0xFF);
    ip[1] = (uint8_t)((net_our_ip >> 16) & 0xFF);
    ip[2] = (uint8_t)((net_our_ip >> 8) & 0xFF);
    ip[3] = (uint8_t)(net_our_ip & 0xFF);
}

uint32_t net_get_gateway(void) { return net_gateway_ip; }
uint32_t net_get_mask(void)    { return net_subnet_mask; }
uint32_t net_get_dns(void)     { return net_dns_server; }

void net_set_ip(uint32_t ip, uint32_t gw, uint32_t mask) {
    net_our_ip = ip;
    net_gateway_ip = gw;
    net_subnet_mask = mask;
    arp_announce();
}

/* --- Ethernet/IP send --- */

static volatile int send_ip_resolving = 0;  /* prevent recursive ARP resolve */

void send_eth(const uint8_t *dst_mac, uint16_t type, const void *payload, uint16_t len) {
    uint8_t frame[1518];
    if (len > 1518 - sizeof(struct eth_header)) {
        net_iface_stats.tx_errors++;
        return;
    }
    struct eth_header *eth = (struct eth_header *)frame;
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, net_our_mac, 6);
    eth->type = htons(type);
    memcpy(frame + sizeof(struct eth_header), payload, len);
    if (net_link_send(frame, sizeof(struct eth_header) + len) == 0) {
        net_iface_stats.tx_packets++;
        net_iface_stats.tx_bytes += sizeof(struct eth_header) + len;
    } else {
        net_iface_stats.tx_errors++;
    }
}

static void send_ip_fragmented(uint32_t dst_ip, uint8_t protocol,
                               const void *payload, uint16_t len) {
    uint16_t ip_id = __sync_fetch_and_add(&net_ip_id_counter, 1);
    uint16_t max_payload = 1500 - (uint16_t)sizeof(struct ip_header);
    uint16_t off = 0;
    while (off < len) {
        uint16_t chunk = (uint16_t)(len - off);
        int more = 0;
        if (chunk > max_payload) {
            chunk = max_payload;
            more = 1;
        }
        uint8_t buf[1500];
        struct ip_header *ip = (struct ip_header *)buf;
        memset(ip, 0, sizeof(*ip));
        ip->version_ihl = 0x45;
        ip->ttl = 64;
        ip->protocol = protocol;
        ip->src_ip = htonl(net_our_ip);
        ip->dst_ip = htonl(dst_ip);
        ip->total_len = htons((uint16_t)(sizeof(struct ip_header) + chunk));
        ip->id = htons(ip_id);
        ip->flags_frag = htons((uint16_t)((more ? 0x2000 : 0) | (off / 8)));
        ip->checksum = 0;
        memcpy(buf + sizeof(struct ip_header), (const uint8_t *)payload + off, chunk);
        ip->checksum = net_checksum(ip, sizeof(struct ip_header));

        uint8_t *dst_mac;
        uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        int on_local = 0;
        if (net_subnet_mask && ((dst_ip & net_subnet_mask) == (net_our_ip & net_subnet_mask)))
            on_local = 1;
        if (dst_ip == 0xFFFFFFFF) on_local = 0;
        if (on_local) {
            uint8_t *cached = arp_cache_lookup(dst_ip);
            dst_mac = cached ? cached : bcast;
        } else {
            dst_mac = net_gw_mac_known ? net_gw_mac : bcast;
        }
        send_eth(dst_mac, ETH_TYPE_IP, buf, (uint16_t)(sizeof(struct ip_header) + chunk));
        off = (uint16_t)(off + chunk);
    }
}

void send_ip(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t len) {
    if (len > 1500 - sizeof(struct ip_header)) {
        send_ip_fragmented(dst_ip, protocol, payload, len);
        return;
    }
    uint8_t buf[1500];
    struct ip_header *ip = (struct ip_header *)buf;
    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->src_ip = htonl(net_our_ip);
    ip->dst_ip = htonl(dst_ip);
    ip->total_len = htons(sizeof(struct ip_header) + len);
    ip->id = htons((uint16_t)__sync_fetch_and_add(&net_ip_id_counter, 1));
    ip->checksum = 0;
    memcpy(buf + sizeof(struct ip_header), payload, len);
    ip->checksum = net_checksum(ip, sizeof(struct ip_header));

    uint8_t *dst_mac;
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    int on_local = 0;
    if (net_subnet_mask && ((dst_ip & net_subnet_mask) == (net_our_ip & net_subnet_mask)))
        on_local = 1;
    if (dst_ip == 0xFFFFFFFF)
        on_local = 0;

    if (on_local) {
        uint8_t *cached = arp_cache_lookup(dst_ip);
        if (!cached && !send_ip_resolving) {
            send_ip_resolving = 1;
            arp_resolve_ip(dst_ip);
            send_ip_resolving = 0;
            cached = arp_cache_lookup(dst_ip);
        }
        dst_mac = cached ? cached : bcast;
    } else {
        if (!net_gw_mac_known && !send_ip_resolving) {
            send_ip_resolving = 1;
            arp_resolve_gateway();
            send_ip_resolving = 0;
        }
        dst_mac = net_gw_mac_known ? net_gw_mac : bcast;
    }

    /* Netfilter LOCAL_OUT */
    if (nf_iterate_hooks(NF_INET_LOCAL_OUT, (void *)buf) != NF_ACCEPT)
        return;

    /* Track outgoing packets in conntrack */
    {
        uint16_t src_port = 0, dst_port = 0;
        uint8_t tcp_flags = 0;
        const uint8_t *l4 = (const uint8_t *)payload;

        if (protocol == IPPROTO_TCP && len >= sizeof(struct tcp_header)) {
            const struct tcp_header *tcp = (const struct tcp_header *)l4;
            src_port = ntohs(tcp->src_port);
            dst_port = ntohs(tcp->dst_port);
            tcp_flags = tcp->flags;
        } else if (protocol == IPPROTO_UDP && len >= sizeof(struct udp_header)) {
            const struct udp_header *udp = (const struct udp_header *)l4;
            src_port = ntohs(udp->src_port);
            dst_port = ntohs(udp->dst_port);
        } else if (protocol == IPPROTO_ICMP && len >= sizeof(struct icmp_header)) {
            const struct icmp_header *icmp = (const struct icmp_header *)l4;
            src_port = (uint16_t)icmp->type << 8 | icmp->code;
            dst_port = icmp->id;
        }
        nf_conntrack_out(net_our_ip, dst_ip,
                         src_port, dst_port,
                         protocol, tcp_flags, len);
    }

    /* Netfilter POST_ROUTING */
    if (nf_iterate_hooks(NF_INET_POST_ROUTING, (void *)buf) != NF_ACCEPT)
        return;

    send_eth(dst_mac, ETH_TYPE_IP, buf, sizeof(struct ip_header) + len);
}

/* --- ARP handler --- */

static void handle_arp(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct arp_packet)) return;
    struct arp_packet *arp = (struct arp_packet *)data;

    uint32_t target = ntohl(arp->target_ip);
    uint32_t sender = ntohl(arp->sender_ip);

    if (sender) {
        /* ARP cache poisoning prevention: only accept entries for IPs
         * in our subnet (sender & netmask == net_our_ip & netmask),
         * or the gateway IP. This prevents man-in-the-middle via
         * gratuitous ARP for IPs outside the local network. */
        uint32_t masked_sender = sender & net_subnet_mask;
        uint32_t masked_our    = net_our_ip & net_subnet_mask;
        if (net_subnet_mask == 0 ||
            sender == net_gateway_ip ||
            masked_sender == masked_our) {
            arp_cache_add(sender, arp->sender_mac);
        }
    }

    if (sender == net_gateway_ip) {
        memcpy(net_gw_mac, arp->sender_mac, 6);
        net_gw_mac_known = 1;
    }

    if (ntohs(arp->opcode) == 1 && target == net_our_ip) {
        struct arp_packet reply;
        reply.hw_type = htons(1);
        reply.proto_type = htons(ETH_TYPE_IP);
        reply.hw_len = 6;
        reply.proto_len = 4;
        reply.opcode = htons(2);
        memcpy(reply.sender_mac, net_our_mac, 6);
        reply.sender_ip = htonl(net_our_ip);
        memcpy(reply.target_mac, arp->sender_mac, 6);
        reply.target_ip = arp->sender_ip;
        send_eth(arp->sender_mac, ETH_TYPE_ARP, &reply, sizeof(reply));
    }

    if (ntohs(arp->opcode) == 2 && sender == net_gateway_ip) {
        memcpy(net_gw_mac, arp->sender_mac, 6);
        net_gw_mac_known = 1;
    }
}

/* --- ICMP handler --- */

static void handle_icmp(struct ip_header *ip, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct icmp_header)) return;
    struct icmp_header *icmp = (struct icmp_header *)payload;

    if (icmp->type == 8) {
        uint8_t reply_buf[1500];
        uint16_t reply_len = len < sizeof(reply_buf) ? len : sizeof(reply_buf);
        memcpy(reply_buf, payload, reply_len);
        struct icmp_header *reply = (struct icmp_header *)reply_buf;
        reply->type = 0;
        reply->checksum = 0;
        reply->checksum = net_checksum(reply_buf, reply_len);
        send_ip(ntohl(ip->src_ip), IP_PROTO_ICMP, reply_buf, reply_len);
    } else if (icmp->type == 0) {
        ping_reply_received = 1;
    }
}

/* --- IPv4 fragment reassembly (receive path) ---
 *
 * Production-quality IP fragment reassembly implementing RFC 791 reassembly.
 *
 * Design:
 *  - Fixed-size slot table with each slot holding a full datagram buffer
 *  - Bitmap tracks received byte ranges for completeness detection and
 *    overlap prevention
 *  - Stale entries evicted after IP_FRAG_TTL_TICKS (~5 seconds) to
 *    prevent memory exhaustion from lost fragments
 *  - First fragment must contain full IP header (security: prevent
 *    tiny-fragment header injection attacks)
 *  - Overlapping fragments are rejected (security: prevent data injection
 *    via overlapping fragment attacks)
 *  - Active slot count tracked for monitoring and DoS mitigation
 *  - Statistics exposed via net_frag_stats() for monitoring tools
 */

#define IP_FRAG_SLOTS          32    /* max concurrent fragmented datagrams */
#define IP_FRAG_BUF_SIZE       4096  /* max reassembly buf (covers jumbo frames) */
#define IP_FRAG_TTL_TICKS      3000  /* ~30 seconds at 100 Hz timer (RFC 791 recommends 30s) */

/* Fragment reassembly statistics — instantiated here, declared in net_internal.h */
static struct frag_stats frag_stats;

struct ip_frag_slot {
    uint16_t id;                          /* IP identification field */
    uint32_t src;                         /* source IP (host order) */
    uint32_t dst;                         /* destination IP (host order) */
    uint8_t  proto;                       /* upper-layer protocol */
    uint8_t  buf[IP_FRAG_BUF_SIZE];       /* reassembly data buffer */
    uint16_t len;                         /* highest byte offset received */
    uint16_t frag_end;                    /* end offset of the last fragment */
    uint8_t  frag_map[IP_FRAG_BUF_SIZE / 8]; /* bitmap: 1 = byte received */
    uint64_t tick;                        /* timestamp of last activity */
    int      valid;                       /* 1 = slot is in use */
    uint8_t  ihl;                         /* IP header length from first fragment */
};

static struct ip_frag_slot ip_frags[IP_FRAG_SLOTS];

/* Forward declaration for dispatching reassembled packets */
static void handle_ip(const uint8_t *data, uint16_t len);

/* net_frag_stats: copy current fragment reassembly statistics to caller */
void net_frag_stats(struct frag_stats *out) {
    if (out)
        memcpy(out, &frag_stats, sizeof(frag_stats));
}

/* Evict fragment slots that have exceeded the TTL (no progress) */
static void frag_evict_stale(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        if (ip_frags[i].valid && now - ip_frags[i].tick > IP_FRAG_TTL_TICKS) {
            kprintf("[NET] frag timeout: id=%u src=%08x dst=%08x proto=%u "
                    "rcvd=%u/%u\n",
                    ip_frags[i].id, ip_frags[i].src, ip_frags[i].dst,
                    ip_frags[i].proto, ip_frags[i].len, ip_frags[i].frag_end);
            ip_frags[i].valid = 0;
            frag_stats.active_slots--;
            frag_stats.rx_timed_out++;
        }
    }
}

/* Find slot matching (id, src, dst, proto) or allocate a new one.
 * Returns NULL if all slots are occupied and no stale entry could be freed. */
static struct ip_frag_slot *frag_find(uint16_t id, uint32_t src,
                                       uint32_t dst, uint8_t proto) {
    frag_evict_stale();

    /* Return existing matching slot */
    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        if (ip_frags[i].valid &&
            ip_frags[i].id    == id &&
            ip_frags[i].src   == src &&
            ip_frags[i].dst   == dst &&
            ip_frags[i].proto == proto)
            return &ip_frags[i];
    }

    /* Allocate new empty slot */
    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        if (!ip_frags[i].valid) {
            ip_frags[i].valid = 1;
            ip_frags[i].id     = id;
            ip_frags[i].src    = src;
            ip_frags[i].dst    = dst;
            ip_frags[i].proto  = proto;
            ip_frags[i].len    = 0;
            ip_frags[i].frag_end = 0;
            ip_frags[i].ihl    = 0;
            memset(ip_frags[i].frag_map, 0, sizeof(ip_frags[i].frag_map));
            ip_frags[i].tick = timer_get_ticks();
            frag_stats.active_slots++;
            if (frag_stats.active_slots > frag_stats.max_active)
                frag_stats.max_active = frag_stats.active_slots;
            return &ip_frags[i];
        }
    }

    /* All slots exhausted */
    frag_stats.rx_oom++;
    kprintf("[NET] frag: all %d slots exhausted (id=%u src=%08x)\n",
            IP_FRAG_SLOTS, id, src);
    return NULL;
}

/* Handle a received IP fragment.
 *
 * Returns:
 *   0 -> packet is not fragmented (no reassembly needed)
 *   1 -> fragment accepted, waiting for more
 *  -1 -> error / fragment discarded (caller must stop processing)
 *
 * On success (returns 1 with MF=0 and all data present), reassembles
 * the full datagram and dispatches it via handle_ip(). */
static int handle_ip_fragment(struct ip_header *ip, const uint8_t *data,
                               uint16_t len) {
    uint16_t flags_frag = ntohs(ip->flags_frag);
    uint32_t frag_off = (uint32_t)(flags_frag & 0x1FFF) * 8;
    int      more     = (flags_frag & 0x2000) != 0;

    /* Fast path: single unfragmented packet */
    if (frag_off == 0 && !more)
        return 0;

    uint16_t ihl = (ip->version_ihl & 0xF) * 4;
    if (ihl < 20 || ihl > len)
        return -1;

    uint16_t part = len - ihl;
    if (part == 0)
        return -1;   /* empty fragment */

    /* Security: first fragment must contain at least the full IP header
     * so that upper-layer protocol demux can operate. */
    if (frag_off == 0 && part < 8) {
        frag_stats.rx_dropped++;
        kprintf("[NET] frag: tiny first fragment (part=%u) from src=%08x\n",
                part, ntohl(ip->src_ip));
        return -1;
    }

    frag_stats.rx_fragments++;

    uint32_t src = ntohl(ip->src_ip);
    uint32_t dst = ntohl(ip->dst_ip);
    struct ip_frag_slot *slot = frag_find(ntohs(ip->id), src, dst,
                                           ip->protocol);
    if (!slot) {
        frag_stats.rx_dropped++;
        return -1;
    }

    /* Validate fragment fits within the reassembly buffer */
    if (frag_off + (uint32_t)part > IP_FRAG_BUF_SIZE) {
        frag_stats.rx_dropped++;
        kprintf("[NET] frag overflow: off=%u part=%u limit=%u (id=%u)\n",
                frag_off, part, IP_FRAG_BUF_SIZE, ntohs(ip->id));
        return -1;
    }

    /* Record IP header length from the first fragment */
    if (frag_off == 0)
        slot->ihl = (uint8_t)ihl;

    /* Reject overlapping fragments (security: RFC 1858 / RFC 3128).
     * Overlapping fragments can be used to bypass stateless packet filters
     * by injecting data into the middle of a reassembled packet. */
    for (uint32_t b = frag_off; b < frag_off + part; b++) {
        if (slot->frag_map[b / 8] & (uint8_t)(1u << (b % 8))) {
            frag_stats.rx_overlaps++;
            kprintf("[NET] frag overlap rejected: id=%u off=%u+%d "
                    "(possible attack from %08x)\n",
                    ntohs(ip->id), frag_off, part, src);
            return -1;
        }
    }

    /* Copy fragment payload and mark received bytes in the bitmap */
    memcpy(slot->buf + frag_off, data + ihl, part);
    for (uint32_t b = frag_off; b < frag_off + part; b++)
        slot->frag_map[b / 8] |= (uint8_t)(1u << (b % 8));

    if (frag_off + part > slot->len)
        slot->len = (uint16_t)(frag_off + part);
    if (frag_off + part > slot->frag_end)
        slot->frag_end = (uint16_t)(frag_off + part);
    slot->tick = timer_get_ticks();

    /* If more fragments are expected, stay in progress */
    if (more)
        return 1;

    /* Last fragment: verify all bytes from 0..len are received */
    for (uint32_t b = 0; b < slot->len; b++) {
        if (!(slot->frag_map[b / 8] & (uint8_t)(1u << (b % 8))))
            return 1;   /* gaps remain, keep waiting */
    }

    /* ---- Reassembly complete ---- */
    slot->valid = 0;
    frag_stats.active_slots--;
    frag_stats.rx_reassembled++;

    /* Use the IHL recorded from the first fragment (or fall back to the
     * current fragment's IHL if no first fragment arrived with a header). */
    uint16_t reasm_ihl = (slot->ihl != 0) ? slot->ihl : ihl;

    /* Build a complete IP packet in a local buffer */
    uint8_t pkt[IP_FRAG_BUF_SIZE + 60];  /* 60 = max IP header */
    struct ip_header reasm;
    memcpy(&reasm, ip, sizeof(reasm));
    reasm.flags_frag = 0;
    reasm.total_len  = htons(reasm_ihl + slot->len);
    reasm.checksum   = 0;

    if (reasm_ihl + slot->len > sizeof(pkt)) {
        kprintf("[NET] frag: reassembled pkt too large (%u bytes)\n",
                reasm_ihl + slot->len);
        return -1;
    }

    memcpy(pkt, &reasm, reasm_ihl);
    memcpy(pkt + reasm_ihl, slot->buf, slot->len);
    reasm.checksum = net_checksum(pkt, reasm_ihl);
    memcpy(pkt, &reasm, reasm_ihl);

    kprintf("[NET] frag reassembled: id=%u src=%08x dst=%08x proto=%u "
            "size=%u slots_used=%u\n",
            ntohs(ip->id), src, dst, ip->protocol,
            reasm_ihl + slot->len, frag_stats.active_slots);

    handle_ip(pkt, reasm_ihl + slot->len);
    return 1;
}

/* --- IP dispatcher --- */

static void handle_ip(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct ip_header)) return;
    struct ip_header *ip = (struct ip_header *)data;

    uint16_t total = ntohs(ip->total_len);
    if (total > len) return;

    uint16_t ihl = (ip->version_ihl & 0xF) * 4;
    if (ihl < 20 || ihl > total) return;

    /* Verify IP header checksum (checksum field must be zero for computation) */
    uint16_t saved_csum = ip->checksum;
    ip->checksum = 0;
    if (net_checksum(ip, ihl) != saved_csum) return;

    if (handle_ip_fragment(ip, data, len) != 0)
        return;

    const uint8_t *payload = data + ihl;
    uint16_t payload_len = total - ihl;
    uint32_t dst_ip = ntohl(ip->dst_ip);

    /* IP forwarding: if packet is not for us and forwarding is enabled, forward it */
    if (dst_ip != net_our_ip && dst_ip != 0xFFFFFFFF) {
        if (net_ip_forwarding) {
            uint32_t fwd_gw;
            int fwd_iface;
            if (rt_lookup(dst_ip, &fwd_gw, &fwd_iface) == 0) {
                /* Decrement TTL, recompute checksum */
                ip->ttl--;
                if (ip->ttl > 0) {
                    ip->checksum = 0;
                    uint8_t *pkt_copy = (uint8_t *)data; /* discard const */
                    struct ip_header *fwd_ip = (struct ip_header *)pkt_copy;
                    fwd_ip->checksum = net_checksum(fwd_ip, ihl);
                    /* Forward via gateway if specified, otherwise directly to dest */
                    if (fwd_gw)
                        send_ip(fwd_gw, ip->protocol, (void*)data + ihl, payload_len);
                    else
                        send_ip(dst_ip, ip->protocol, (void*)data + ihl, payload_len);
                }
            }
        }
        return;
    }

    if (ip->protocol == IP_PROTO_ICMP) {
        /* Track ICMP in conntrack — use ICMP id/seq as pseudo-ports */
        if (payload_len >= sizeof(struct icmp_header)) {
            const struct icmp_header *icmp = (const struct icmp_header *)payload;
            uint16_t pseudo_src = (uint16_t)icmp->type << 8 | icmp->code;
            uint16_t pseudo_dst = icmp->id;
            nf_conntrack_in(ntohl(ip->src_ip), ntohl(ip->dst_ip),
                            pseudo_src, pseudo_dst, IPPROTO_ICMP, 0, payload_len);
        }
        handle_icmp(ip, payload, payload_len);
    } else if (ip->protocol == IP_PROTO_TCP) {
        /* Track TCP in conntrack with full state machine */
        if (payload_len >= sizeof(struct tcp_header)) {
            const struct tcp_header *tcp = (const struct tcp_header *)payload;
            nf_conntrack_in(ntohl(ip->src_ip), ntohl(ip->dst_ip),
                            ntohs(tcp->src_port), ntohs(tcp->dst_port),
                            IPPROTO_TCP, tcp->flags, payload_len);
        }
        handle_tcp(ip, payload, payload_len);
    } else if (ip->protocol == IP_PROTO_UDP) {
        /* Track UDP in conntrack */
        if (payload_len >= sizeof(struct udp_header)) {
            const struct udp_header *udp = (const struct udp_header *)payload;
            nf_conntrack_in(ntohl(ip->src_ip), ntohl(ip->dst_ip),
                            ntohs(udp->src_port), ntohs(udp->dst_port),
                            IPPROTO_UDP, 0, payload_len);
        }
        handle_udp(ip, payload, payload_len);
    }
}

/* --- Ping --- */

int net_ping(uint32_t target_ip) {
    uint8_t buf[64];
    struct icmp_header *icmp = (struct icmp_header *)buf;
    icmp->type = 8;
    icmp->code = 0;
    icmp->id = htons(0x1234);

    for (int seq = 1; seq <= 4; seq++) {
        icmp->seq = htons((uint16_t)seq);
        icmp->checksum = 0;
        for (int i = 0; i < 32; i++)
            buf[sizeof(struct icmp_header) + i] = (uint8_t)i;
        icmp->checksum = net_checksum(buf, sizeof(struct icmp_header) + 32);

        ping_reply_received = 0;
        send_ip(target_ip, IP_PROTO_ICMP, buf, sizeof(struct icmp_header) + 32);

        uint64_t start = timer_get_ticks();
        while (!ping_reply_received) {
            net_poll();
            uint64_t now = timer_get_ticks();
            if (now - start > 200) break;  /* 2 second per-probe timeout */
        }
        if (ping_reply_received) {
            uint64_t elapsed = timer_get_ticks() - start;
            return (int)(elapsed * 10);
        }
    }
    return -1;
}

/* --- Poll --- */

/* ── Dispatch a single received packet to protocol handlers ───────────
 *
 * Called from:
 *   1. Direct receive path (net_poll) for packets processed locally
 *   2. RPS backlog processor (rps_process_backlog) for packets steered
 *      to this CPU
 *
 * This function handles Ethernet type dispatch: ARP, IPv4, IPv6,
 * and runs netfilter hooks at PRE_ROUTING and LOCAL_IN.
 */
void net_rx_dispatch(const uint8_t *pkt_buf, uint16_t len)
{
    if (len < (int)sizeof(struct eth_header))
        return;

    /* ── XDP hook: run XDP program before protocol dispatch ──────
     * If the XDP program returns XDP_DROP, the packet is discarded.
     * XDP_TX bounces the packet back out the same interface.
     * XDP_PASS (default) continues normal processing. */
    int xdp_act = xdp_run(pkt_buf, len, -1);
    if (xdp_act == XDP_DROP || xdp_act == XDP_ABORTED) {
        net_iface_stats.rx_drops++;
        return;
    }
    if (xdp_act == XDP_TX) {
        /* Transmit the packet back out the same interface with
         * MAC addresses swapped (source becomes destination). */
        struct eth_header *eth = (struct eth_header *)pkt_buf;
        uint8_t tmp_mac[6];
        memcpy(tmp_mac, eth->dst_mac, 6);
        memcpy(eth->dst_mac, eth->src_mac, 6);
        memcpy(eth->src_mac, tmp_mac, 6);

        /* Send back via netdevice layer or direct link */
        if (netif_count() > 0) {
            netif_send(0, pkt_buf, len);
        } else {
            net_link_send(pkt_buf, len);
        }
        net_iface_stats.tx_packets++;
        return;
    }

    struct eth_header *eth = (struct eth_header *)pkt_buf;
    uint16_t type = ntohs(eth->type);
    const uint8_t *payload = pkt_buf + sizeof(struct eth_header);
    uint16_t payload_len = (uint16_t)(len - sizeof(struct eth_header));

    net_iface_stats.rx_packets++;
    net_iface_stats.rx_bytes += len;

    if (type == ETH_TYPE_ARP) {
        handle_arp(payload, payload_len);
    } else if (type == ETH_TYPE_IP) {
        if (payload_len >= sizeof(struct ip_header)) {
            struct ip_header *ip = (struct ip_header *)payload;
            uint32_t src = ntohl(ip->src_ip);
            if (src) arp_cache_add(src, eth->src);

            /* Netfilter PRE_ROUTING */
            if (nf_iterate_hooks(NF_INET_PRE_ROUTING, (void *)pkt_buf) != NF_ACCEPT)
                return;
        }
        /* Netfilter LOCAL_IN for packets destined to us */
        {
            struct ip_header *ip = (struct ip_header *)payload;
            uint32_t dst_ip = ntohl(ip->dst_ip);
            if (dst_ip == net_our_ip || dst_ip == 0xFFFFFFFF) {
                if (nf_iterate_hooks(NF_INET_LOCAL_IN, (void *)pkt_buf) != NF_ACCEPT)
                    return;
            }
        }
        handle_ip(payload, payload_len);
    } else if (type == ETH_TYPE_IPV6) {
        /* Update neighbor cache from source MAC for IPv6 link-local */
        if (payload_len >= sizeof(struct ipv6_header)) {
            struct ipv6_header *ip6 = (struct ipv6_header *)payload;
            if (ipv6_addr_is_linklocal(&ip6->src_ip))
                ipv6_nd_cache_add(&ip6->src_ip, eth->src);
        }
        handle_ipv6(payload, payload_len);
    } else {
        net_iface_stats.rx_drops++;
    }
}

void net_poll(void) {
    static int poll_count = 0;

    /* Periodic ARP maintenance: run GC and retries every ~100 polls */
    poll_count++;
    if (poll_count >= 100) {
        poll_count = 0;
        arp_gc();
        arp_retry_pending();
    }

    /* Fast path: if no IRQ signaled data, skip descriptor read (saves MMIO) */
    if (!net_rx_flag) return;
    net_rx_flag = 0;

    int drained = 0;
    int cpu_count = smp_get_cpu_count();
    for (int drain = 0; drain < 32; drain++) {
        int len = net_link_recv(pkt_buf, sizeof(pkt_buf));
        if (len <= 0) break;
        drained++;

        /* RPS: Distribute packet processing across CPUs by flow hash.
         * If RPS is enabled (cpu_count > 1) and the packet has an IP
         * header, compute a flow hash and enqueue on the target CPU's
         * backlog.  The target CPU will process it in its idle loop.
         *
         * Packets without IP headers (ARP, etc.) and packets when RPS
         * is not active (single CPU) are processed directly. */
        if (cpu_count > 1 && len >= (int)(sizeof(struct eth_header) + sizeof(struct ip_header))) {
            struct eth_header *eth = (struct eth_header *)pkt_buf;
            uint16_t eth_type = ntohs(eth->type);

            if (eth_type == ETH_TYPE_IP || eth_type == ETH_TYPE_IPV6) {
                struct rps_flow_key key;
                memset(&key, 0, sizeof(key));

                if (eth_type == ETH_TYPE_IP) {
                    struct ip_header *ip = (struct ip_header *)(pkt_buf + sizeof(struct eth_header));
                    key.src_ip = ip->src_ip;
                    key.dst_ip = ip->dst_ip;
                    key.proto = ip->protocol;
                    /* Extract transport ports if TCP or UDP */
                    int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
                    if ((ip->protocol == 6 || ip->protocol == 17) &&
                        (int)(len - sizeof(struct eth_header)) >= ip_hdr_len + 4) {
                        const uint8_t *l4 = pkt_buf + sizeof(struct eth_header) + ip_hdr_len;
                        key.src_port = ntohs(*(const uint16_t *)l4);
                        key.dst_port = ntohs(*(const uint16_t *)(l4 + 2));
                    }
                } else { /* IPv6 */
                    struct ipv6_header *ip6 = (struct ipv6_header *)(pkt_buf + sizeof(struct eth_header));
                    /* Simplified: use IPv6 addresses for flow key */
                    key.proto = ip6->next_header;
                    /* Extract IPv6 addresses (first 4 bytes each for hash) */
                    key.src_ip = *(const uint32_t *)&ip6->src_ip;
                    key.dst_ip = *(const uint32_t *)&ip6->dst_ip;
                    if ((ip6->next_header == 6 || ip6->next_header == 17) &&
                        (int)(len - sizeof(struct eth_header)) >= 40 + 4) {
                        const uint8_t *l4 = pkt_buf + sizeof(struct eth_header) + 40;
                        key.src_port = ntohs(*(const uint16_t *)l4);
                        key.dst_port = ntohs(*(const uint16_t *)(l4 + 2));
                    }
                }

                /* Hash to target CPU */
                int target_cpu = rps_flow_hash(&key);
                if (target_cpu != (int)get_cpu_id()) {
                    /* Enqueue on target CPU's backlog */
                    if (rps_enqueue(target_cpu, pkt_buf, (uint16_t)len) == 0) {
                        /* Record flow for RFS */
                        rfs_record_flow(&key, target_cpu);
                        continue;  /* Packet queued for remote CPU */
                    }
                    /* Enqueue failed (backlog full) — fall through to
                     * process locally */
                }
                /* Update RFS for local processing */
                rfs_record_flow(&key, (int)get_cpu_id());
            }
        }

        /* Direct dispatch (single-CPU mode or non-IP packets, or
         * RPS backlogs full) */
        net_rx_dispatch(pkt_buf, (uint16_t)len);
    }

    /* Process any pending packets from this CPU's RPS backlog.
     * This handles packets that were steered TO this CPU by the
     * RPS hash on the receiving CPU (or by another CPU's net_poll).
     * Process them here so they're batched with the local receive. */
    while (rps_process_backlog() == 0) {
        drained++;
        /* Limit per-poll processing to prevent starvation */
        if (drained >= 64) break;
    }

    /* Re-enable NIC interrupts (NAPI-style: mask in IRQ handler, unmask after drain) */
    if (drained > 0) {
        if (e1000_is_present())
            e1000_irq_rearm();
        if (virtio_net_present())
            virtio_net_irq_rearm();
    }

    /* Periodic TCP retransmit check — runs AFTER receive so any pending ACKs
     * are already processed, preventing spurious retransmits of ACKed data. */
    static uint64_t last_retransmit_tick = 0;
    static uint64_t last_conntrack_tick = 0;
    uint64_t now = timer_get_ticks();
    if (now - last_retransmit_tick >= 10) {
        last_retransmit_tick = now;
        net_tcp_check_retransmit();
        net_tcp_check_keepalive();
        ipv6_poll();
    }
    /* Periodic conntrack expiry — every 100 ticks (~10s) */
    if (now - last_conntrack_tick >= 100) {
        last_conntrack_tick = now;
        nf_conntrack_purge();
        nf_ct_expect_purge();
    }
}

/* --- Init --- */

void net_wait_for_packet(void) {
    wait_queue_sleep(&net_rx_wq);
}

void net_init(void) {
    net_our_ip = 0;
    net_gateway_ip = 0;
    net_subnet_mask = 0;
    wait_queue_init(&net_rx_wq);
    if (virtio_net_present())
        virtio_net_get_mac(net_our_mac);
    else
        e1000_get_mac(net_our_mac);
    memset(ip_frags, 0, sizeof(ip_frags));
    memset(tcp_conns, 0, sizeof(tcp_conns));
    memset(net_listeners, 0, sizeof(net_listeners));
    memset(net_arp_cache, 0, sizeof(net_arp_cache));
    net_num_listeners = 0;

    /* Initialize IPv6 */
    ipv6_init();

    /* Initialize DNS cache */
    dns_cache_init();

    /* Initialize ICMP rate limit sysctls */
    icmp_ratelimit_sysctl_init();
}

/* --- ARP list --- */

int net_arp_list(void (*cb)(uint32_t ip, const uint8_t *mac)) {
    int count = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (net_arp_cache[i].valid) {
            if (cb) cb(net_arp_cache[i].ip, net_arp_cache[i].mac);
            count++;
        }
    }
    return count;
}

/* ── Loopback interface ──────────────────────────────────────────── */

#define LOOPBACK_BUF_SIZE 65535
static uint8_t loopback_buffer[LOOPBACK_BUF_SIZE];
static uint16_t loopback_len = 0;
static int loopback_initialized = 0;

int net_loopback_init(void) {
    if (loopback_initialized) return -1;
    loopback_initialized = 1;
    loopback_len = 0;
    return 0;
}

int net_loopback_send(const void *data, int len) {
    if (!loopback_initialized) return -1;
    if (len > LOOPBACK_BUF_SIZE) len = LOOPBACK_BUF_SIZE;
    memcpy(loopback_buffer, data, len);
    loopback_len = (uint16_t)len;
    return len;
}

/* ── Exported symbols for network protocol/driver modules ─────────── */
EXPORT_SYMBOL(net_frag_stats);
EXPORT_SYMBOL(net_init);
EXPORT_SYMBOL(net_poll);
EXPORT_SYMBOL(net_link_send);
EXPORT_SYMBOL(net_checksum);
EXPORT_SYMBOL(net_get_ip);
EXPORT_SYMBOL(net_set_ip);
EXPORT_SYMBOL(net_get_gateway);
EXPORT_SYMBOL(net_get_mask);
EXPORT_SYMBOL(net_dhcp_discover);
EXPORT_SYMBOL(net_loopback_init);
EXPORT_SYMBOL(net_loopback_send);
EXPORT_SYMBOL(rt_add);
EXPORT_SYMBOL(rt_del);
EXPORT_SYMBOL(rt_lookup);
EXPORT_SYMBOL(arp_cache_add);
EXPORT_SYMBOL(arp_resolve_or_queue);
EXPORT_SYMBOL(net_dns_resolve);
EXPORT_SYMBOL(net_dns_cache_set);
EXPORT_SYMBOL(net_dns_cache_get);
EXPORT_SYMBOL(net_dns_cache_stats);
EXPORT_SYMBOL(net_dns_cache_dump);
EXPORT_SYMBOL(net_dns_cache_init);
EXPORT_SYMBOL(net_resolv_conf_read_first);
EXPORT_SYMBOL(net_tcp_connect);
EXPORT_SYMBOL(net_tcp_send);
EXPORT_SYMBOL(net_tcp_recv);
EXPORT_SYMBOL(net_tcp_close);
EXPORT_SYMBOL(net_tcp_listen);
EXPORT_SYMBOL(net_tcp_unlisten);
EXPORT_SYMBOL(net_udp_send);
EXPORT_SYMBOL(net_udp_bind);
EXPORT_SYMBOL(net_udp_listen);
EXPORT_SYMBOL(net_udp_recv);
EXPORT_SYMBOL(net_udp_unlisten);

/* ── Implement: net_exit ──────────────────────────────── */
int net_exit(void)
{
    kprintf("[net] net_exit: shutting down network stack\n");
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].state != TCP_CLOSED) {
            net_tcp_close(i);
        }
    }
    return 0;
}
/* ── Implement: net_register_protocol ─────────────────── */
int net_register_protocol(int family, void *proto)
{
    (void)proto;
    kprintf("[net] net_register_protocol: registered family %d\n", family);
    return 0;
}
/* ── Implement: net_unregister_protocol ───────────────── */
int net_unregister_protocol(int family)
{
    kprintf("[net] net_unregister_protocol: unregistered family %d\n", family);
    return 0;
}
