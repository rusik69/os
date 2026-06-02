/* net.c — Core networking: state, ethernet/IP, ARP, ICMP, poll, init */

#include "net_internal.h"
#include "e1000.h"
#include "virtio_net.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "waitqueue.h"
#include "netfilter.h"
#include "export.h"

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

static int net_link_recv(void *buf, uint16_t max_len) {
    if (virtio_net_present()) {
        int n = virtio_net_receive(buf, max_len);
        if (n != 0) return n;
    }
    if (e1000_is_present())
        return e1000_receive(buf, max_len);
    return 0;
}

int net_link_send(const void *data, uint16_t len) {
    if (virtio_net_present()) {
        if (virtio_net_send((const uint8_t *)data, len) < 0)
            return -1;
        return 0;
    }
    return e1000_send(data, len);
}

/* ICMP ping state */
static volatile int ping_reply_received = 0;

/* ── IP routing table ───────────────────────────────────────────── */

int rt_add(uint32_t dst, uint32_t mask, uint32_t gw, int iface) {
    if (rt_num_entries >= RT_MAX_ENTRIES) return -1;
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
    return 0;
}

int rt_del(uint32_t dst, uint32_t mask) {
    for (int i = 0; i < rt_num_entries; i++) {
        if (rt_table[i].dst == dst && rt_table[i].mask == mask) {
            for (int j = i; j < rt_num_entries - 1; j++)
                rt_table[j] = rt_table[j + 1];
            rt_num_entries--;
            return 0;
        }
    }
    return -1;
}

int rt_lookup(uint32_t ip, uint32_t *gw_out, int *iface_out) {
    int best = -1;
    uint32_t best_mask = 0;
    for (int i = 0; i < rt_num_entries; i++) {
        if ((ip & rt_table[i].mask) == (rt_table[i].dst & rt_table[i].mask)) {
            if (rt_table[i].mask > best_mask) {
                best = i;
                best_mask = rt_table[i].mask;
            }
        }
    }
    if (best < 0) return -1;
    if (gw_out)   *gw_out   = rt_table[best].gw;
    if (iface_out) *iface_out = rt_table[best].iface;
    return 0;
}

void rt_flush(void) {
    rt_num_entries = 0;
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
    ip[0] = (net_our_ip >> 24) & 0xFF;
    ip[1] = (net_our_ip >> 16) & 0xFF;
    ip[2] = (net_our_ip >> 8) & 0xFF;
    ip[3] = net_our_ip & 0xFF;
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
    uint16_t ip_id = net_ip_id_counter++;
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
    ip->id = htons(net_ip_id_counter++);
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
        arp_cache_add(sender, arp->sender_mac);
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

/* --- IPv4 fragment reassembly (receive path) --- */

#define IP_FRAG_SLOTS 4
struct ip_frag_slot {
    uint16_t id;
    uint32_t src;
    uint32_t dst;
    uint8_t  proto;
    uint8_t  buf[2048];
    uint16_t len;
    uint16_t expect_off;
    uint16_t frag_end;
    uint8_t  frag_map[256];
    uint64_t tick;
    int      valid;
};

static struct ip_frag_slot ip_frags[IP_FRAG_SLOTS];
#define IP_FRAG_TTL_TICKS 500

static void frag_evict_stale(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        if (ip_frags[i].valid && now - ip_frags[i].tick > IP_FRAG_TTL_TICKS)
            ip_frags[i].valid = 0;
    }
}

static struct ip_frag_slot *frag_find(uint16_t id, uint32_t src, uint32_t dst, uint8_t proto) {
    frag_evict_stale();
    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        if (ip_frags[i].valid && ip_frags[i].id == id &&
            ip_frags[i].src == src && ip_frags[i].dst == dst && ip_frags[i].proto == proto)
            return &ip_frags[i];
    }
    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        if (!ip_frags[i].valid) {
            ip_frags[i].valid = 1;
            ip_frags[i].id = id;
            ip_frags[i].src = src;
            ip_frags[i].dst = dst;
            ip_frags[i].proto = proto;
            ip_frags[i].len = 0;
            ip_frags[i].expect_off = 0;
            ip_frags[i].frag_end = 0;
            memset(ip_frags[i].frag_map, 0, sizeof(ip_frags[i].frag_map));
            ip_frags[i].tick = timer_get_ticks();
            return &ip_frags[i];
        }
    }
    return NULL;
}

static void handle_ip(const uint8_t *data, uint16_t len);

static int handle_ip_fragment(struct ip_header *ip, const uint8_t *data, uint16_t len) {
    uint16_t flags_frag = ntohs(ip->flags_frag);
    uint32_t frag_off = (uint32_t)(flags_frag & 0x1FFF) * 8;
    int more = (flags_frag & 0x2000) != 0;

    if (frag_off == 0 && !more)
        return 0; /* not fragmented */

    uint32_t src = ntohl(ip->src_ip);
    uint32_t dst = ntohl(ip->dst_ip);
    struct ip_frag_slot *slot = frag_find(ntohs(ip->id), src, dst, ip->protocol);
    if (!slot) return -1;

    uint16_t ihl = (ip->version_ihl & 0xF) * 4;
    uint16_t part = len - ihl;
    if (frag_off + (uint32_t)part > sizeof(slot->buf)) return -1;

    /* Reject overlapping fragments (security: prevent data injection attacks) */
    for (uint16_t b = frag_off; b < frag_off + part; b++) {
        if (slot->frag_map[b / 8] & (uint8_t)(1u << (b % 8)))
            return -1;
    }

    memcpy(slot->buf + frag_off, data + ihl, part);
    for (uint32_t b = frag_off; b < frag_off + part; b++)
        slot->frag_map[b / 8] |= (uint8_t)(1u << (b % 8));
    if (frag_off + part > slot->len) slot->len = (uint16_t)(frag_off + part);
    if (frag_off + part > slot->frag_end) slot->frag_end = (uint16_t)(frag_off + part);
    slot->tick = timer_get_ticks();

    if (more) return 1;

    for (uint16_t b = 0; b < slot->len; b++) {
        if (!(slot->frag_map[b / 8] & (uint8_t)(1u << (b % 8))))
            return 1;
    }

    slot->valid = 0;
    struct ip_header reasm;
    memcpy(&reasm, ip, sizeof(reasm));
    reasm.flags_frag = 0;
    reasm.total_len = htons(sizeof(struct ip_header) + slot->len);
    reasm.checksum = 0;
    uint8_t pkt[2048];
    memcpy(pkt, &reasm, ihl);
    memcpy(pkt + ihl, slot->buf, slot->len);
    reasm.checksum = net_checksum(pkt, ihl);
    memcpy(pkt, &reasm, ihl);
    handle_ip(pkt, ihl + slot->len);
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

    if (ip->protocol == IP_PROTO_ICMP)
        handle_icmp(ip, payload, payload_len);
    else if (ip->protocol == IP_PROTO_TCP)
        handle_tcp(ip, payload, payload_len);
    else if (ip->protocol == IP_PROTO_UDP)
        handle_udp(ip, payload, payload_len);
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
    for (int drain = 0; drain < 32; drain++) {
        int len = net_link_recv(pkt_buf, sizeof(pkt_buf));
        if (len <= 0) break;
        drained++;
        if (len >= (int)sizeof(struct eth_header)) {
            struct eth_header *eth = (struct eth_header *)pkt_buf;
            uint16_t type = ntohs(eth->type);
            const uint8_t *payload = pkt_buf + sizeof(struct eth_header);
            uint16_t payload_len = len - sizeof(struct eth_header);
            net_iface_stats.rx_packets++;
            net_iface_stats.rx_bytes += len;
            if (type == ETH_TYPE_ARP)
                handle_arp(payload, payload_len);
            else if (type == ETH_TYPE_IP) {
                if (payload_len >= sizeof(struct ip_header)) {
                    struct ip_header *ip = (struct ip_header *)payload;
                    uint32_t src = ntohl(ip->src_ip);
                    if (src) arp_cache_add(src, eth->src);

                    /* Netfilter PRE_ROUTING */
                    if (nf_iterate_hooks(NF_INET_PRE_ROUTING, (void *)pkt_buf) != NF_ACCEPT)
                        continue;
                }
                /* Netfilter LOCAL_IN for packets destined to us */
                {
                    struct ip_header *ip = (struct ip_header *)payload;
                    uint32_t dst_ip = ntohl(ip->dst_ip);
                    if (dst_ip == net_our_ip || dst_ip == 0xFFFFFFFF) {
                        if (nf_iterate_hooks(NF_INET_LOCAL_IN, (void *)pkt_buf) != NF_ACCEPT)
                            continue;
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
            }
        } else {
            net_iface_stats.rx_drops++;
        }
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
    uint64_t now = timer_get_ticks();
    if (now - last_retransmit_tick >= 10) {
        last_retransmit_tick = now;
        net_tcp_check_retransmit();
        net_tcp_check_keepalive();
        ipv6_poll();
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

#define LOOPBACK_BUF_SIZE 65536
static uint8_t loopback_buffer[LOOPBACK_BUF_SIZE];
static uint16_t loopback_len = 0;
static int loopback_initialized = 0;

int net_loopback_init(void) {
    if (loopback_initialized) return -1;
    loopback_initialized = 1;
    loopback_len = 0;
    return 0;
}

int net_loopback_send(const void *data, uint16_t len) {
    if (!loopback_initialized) return -1;
    if (len > LOOPBACK_BUF_SIZE) len = LOOPBACK_BUF_SIZE;
    memcpy(loopback_buffer, data, len);
    loopback_len = len;
    return len;
}

/* ── Exported symbols for network protocol/driver modules ─────────── */
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
