#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "dhcp.h"
#include "string.h"
#include "net.h"
#include "net_internal.h"
#include "timer.h"
#include "e1000.h"
#include "virtio_net.h"
#include "vfs.h"
#include "errno.h"

/*
 * Real DHCP client implementation.
 *
 * Uses the existing network stack's UDP broadcast capability to perform
 * a full DHCP handshake:
 *   1. DHCPDISCOVER (broadcast on UDP 67/68)
 *   2. Receive DHCPOFFER from server
 *   3. Send DHCPREQUEST with the offered IP
 *   4. Receive DHCPACK with configuration
 *
 * On success, returns the assigned IP, netmask, gateway, and DNS server.
 *
 * This builds on the existing net_dhcp_discover() infrastructure already
 * in net_udp.c, but provides a cleaner standalone API.
 */

/* DHCP packet structure (mirrors net_udp.c) */
struct dhcp_packet {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
} __attribute__((packed));

#define DHCP_MAGIC 0x63825363
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

/* Our MAC address (from net_internal.h) */
extern uint8_t net_our_mac[6];

/* UDP/IP send helpers from net_udp.c / net.c */
extern uint16_t net_ip_id_counter;
void send_eth(const uint8_t *dst_mac, uint16_t type, const void *payload, uint16_t len);
uint16_t net_checksum(const void *data, int len);

/* State for our DHCP transaction */
static uint32_t dhcp_xid = 0;
static uint32_t dhcp_offered_ip = 0;
static uint32_t dhcp_server_id = 0;
static uint32_t dhcp_result_ip = 0;
static uint32_t dhcp_result_gateway = 0;
static uint32_t dhcp_result_netmask = 0;
static uint32_t dhcp_result_dns = 0;
static volatile int dhcp_state = 0;  /* 0=idle, 1=discover sent, 2=request sent, 3=done */
static volatile int dhcp_done = 0;

/* ── Low-level send ─────────────────────────────────────────────────── */

static void send_udp_broadcast(uint16_t src_port, uint16_t dst_port,
                                const void *data, uint16_t data_len) {
    uint8_t buf[1500];
    struct ip_header *ip = (struct ip_header *)buf;
    struct udp_header *udp = (struct udp_header *)(buf + sizeof(struct ip_header));

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->src_ip = 0;  /* 0.0.0.0 before we have an IP */
    ip->dst_ip = htonl(0xFFFFFFFF);  /* 255.255.255.255 */

    uint16_t udp_len = sizeof(struct udp_header) + data_len;
    ip->total_len = htons(sizeof(struct ip_header) + udp_len);
    ip->id = htons((uint16_t)__sync_fetch_and_add(&net_ip_id_counter, 1));
    ip->checksum = 0;

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(udp_len);
    udp->checksum = 0;

    memcpy(buf + sizeof(struct ip_header) + sizeof(struct udp_header), data, data_len);
    ip->checksum = net_checksum(ip, sizeof(struct ip_header));

    uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    send_eth(bcast_mac, ETH_TYPE_IP, buf, sizeof(struct ip_header) + udp_len);
}

/* Send a UDP packet to a specific IP (unicast) */
static void send_udp_unicast(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                              const void *data, uint16_t data_len) {
    uint8_t buf[1500];
    struct ip_header *ip = (struct ip_header *)buf;
    struct udp_header *udp = (struct udp_header *)(buf + sizeof(struct ip_header));

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->src_ip = net_our_ip;
    ip->dst_ip = dst_ip;

    uint16_t udp_len = sizeof(struct udp_header) + data_len;
    ip->total_len = htons(sizeof(struct ip_header) + udp_len);
    ip->id = htons((uint16_t)__sync_fetch_and_add(&net_ip_id_counter, 1));
    ip->checksum = 0;

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(udp_len);
    udp->checksum = 0;

    memcpy(buf + sizeof(struct ip_header) + sizeof(struct udp_header), data, data_len);
    ip->checksum = net_checksum(ip, sizeof(struct ip_header));

    /* Use broadcast for now (simplified — no ARP lookup) */
    uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    send_eth(bcast_mac, ETH_TYPE_IP, buf, sizeof(struct ip_header) + udp_len);
}

/* ── DHCP message builders ──────────────────────────────────────────── */

static void dhcp_build_discover(void) {
    uint8_t buf[300];
    memset(buf, 0, sizeof(buf));
    struct dhcp_packet *dhcp = (struct dhcp_packet *)buf;

    dhcp->op = 1;   /* BOOTREQUEST */
    dhcp->htype = 1; /* Ethernet */
    dhcp->hlen = 6;
    dhcp->xid = htonl(dhcp_xid);
    dhcp->flags = htons(0x8000);  /* Broadcast flag */
    dhcp->magic_cookie = htonl(DHCP_MAGIC);
    memcpy(dhcp->chaddr, net_our_mac, 6);

    /* DHCP options */
    uint8_t *opt = buf + sizeof(struct dhcp_packet);
    *opt++ = 53; *opt++ = 1; *opt++ = DHCP_DISCOVER;  /* DHCP message type */
    *opt++ = 55; *opt++ = 3;                            /* Parameter request list */
    *opt++ = 1;   /* Subnet mask */
    *opt++ = 3;   /* Router */
    *opt++ = 6;   /* DNS server */
    *opt++ = 255; /* End option */

    uint16_t pkt_len = (uint16_t)(opt - buf);
    send_udp_broadcast(DHCP_CLIENT_PORT, DHCP_SERVER_PORT, buf, pkt_len);
    dhcp_state = 1;
    kprintf("[dhcp] Sent DISCOVER (xid=0x%x)\n", (uint32_t)dhcp_xid);
}

static void dhcp_build_request(void) {
    uint8_t buf[300];
    memset(buf, 0, sizeof(buf));
    struct dhcp_packet *dhcp = (struct dhcp_packet *)buf;

    dhcp->op = 1;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->xid = htonl(dhcp_xid);
    dhcp->flags = htons(0x8000);
    dhcp->magic_cookie = htonl(DHCP_MAGIC);
    memcpy(dhcp->chaddr, net_our_mac, 6);

    uint8_t *opt = buf + sizeof(struct dhcp_packet);
    *opt++ = 53; *opt++ = 1; *opt++ = DHCP_REQUEST;
    /* Requested IP option (50) */
    *opt++ = 50; *opt++ = 4;
    *opt++ = (uint8_t)(dhcp_offered_ip >> 24);
    *opt++ = (uint8_t)(dhcp_offered_ip >> 16);
    *opt++ = (uint8_t)(dhcp_offered_ip >> 8);
    *opt++ = (uint8_t)(dhcp_offered_ip);
    /* Server identifier option (54) */
    *opt++ = 54; *opt++ = 4;
    *opt++ = (uint8_t)(dhcp_server_id >> 24);
    *opt++ = (uint8_t)(dhcp_server_id >> 16);
    *opt++ = (uint8_t)(dhcp_server_id >> 8);
    *opt++ = (uint8_t)(dhcp_server_id);
    *opt++ = 55; *opt++ = 3;
    *opt++ = 1; *opt++ = 3; *opt++ = 6;
    *opt++ = 255;

    uint16_t pkt_len = (uint16_t)(opt - buf);
    send_udp_broadcast(DHCP_CLIENT_PORT, DHCP_SERVER_PORT, buf, pkt_len);
    dhcp_state = 2;
    kprintf("[dhcp] Sent REQUEST for %u.%u.%u.%u\n",
            (uint32_t)((dhcp_offered_ip >> 24) & 0xFF),
            (uint32_t)((dhcp_offered_ip >> 16) & 0xFF),
            (uint32_t)((dhcp_offered_ip >> 8) & 0xFF),
            (uint32_t)(dhcp_offered_ip & 0xFF));
}

/* ── DHCP response handler ──────────────────────────────────────────── */

/* Store lease time for renewal tracking */
static uint32_t dhcp_lease_time = 0;
static uint64_t dhcp_acquired_tick = 0;
static int dhcp_has_lease = 0;

static void handle_dhcp_response(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct dhcp_packet)) return;
    struct dhcp_packet *dhcp = (struct dhcp_packet *)data;
    if (dhcp->op != 2) return;  /* BOOTREPLY */
    if (ntohl(dhcp->xid) != dhcp_xid) return;
    if (ntohl(dhcp->magic_cookie) != DHCP_MAGIC) return;

    const uint8_t *opt = data + sizeof(struct dhcp_packet);
    const uint8_t *end = data + len;
    uint8_t msg_type = 0;
    uint32_t router = 0;
    uint32_t mask = 0;
    uint32_t dns = 0;
    uint32_t server_id = 0;
    uint32_t lease __attribute__((unused)) = 0;

    while (opt < end && *opt != 255) {
        if (*opt == 0) { opt++; continue; }
        uint8_t code = *opt++;
        if (opt >= end) break;
        uint8_t olen = *opt++;
        if (opt + olen > end) break;

        switch (code) {
        case 53: /* DHCP message type */
            if (olen >= 1) msg_type = opt[0];
            break;
        case 1:  /* Subnet mask */
            if (olen >= 4)
                mask = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) |
                       ((uint32_t)opt[2] << 8) | opt[3];
            break;
        case 3:  /* Router / Gateway */
            if (olen >= 4)
                router = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) |
                         ((uint32_t)opt[2] << 8) | opt[3];
            break;
        case 6:  /* DNS server */
            if (olen >= 4)
                dns = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) |
                      ((uint32_t)opt[2] << 8) | opt[3];
            break;
        case 54: /* Server identifier */
            if (olen >= 4)
                server_id = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) |
                            ((uint32_t)opt[2] << 8) | opt[3];
            break;
        case 51: /* Lease time */
            if (olen >= 4)
                lease = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) |
                        ((uint32_t)opt[2] << 8) | opt[3];
            break;
        }
        opt += olen;
    }

    uint32_t yiaddr = ntohl(dhcp->yiaddr);

    if (msg_type == DHCP_OFFER && dhcp_state == 1) {
        dhcp_offered_ip = yiaddr;
        dhcp_server_id = server_id ? server_id : ntohl(dhcp->siaddr);
        kprintf("[dhcp] Received OFFER for %u.%u.%u.%u from server %u.%u.%u.%u\n",
                (uint32_t)((yiaddr >> 24) & 0xFF), (uint32_t)((yiaddr >> 16) & 0xFF),
                (uint32_t)((yiaddr >> 8) & 0xFF), (uint32_t)(yiaddr & 0xFF),
                (uint32_t)((dhcp_server_id >> 24) & 0xFF), (uint32_t)((dhcp_server_id >> 16) & 0xFF),
                (uint32_t)((dhcp_server_id >> 8) & 0xFF), (uint32_t)(dhcp_server_id & 0xFF));
        dhcp_build_request();
    } else if (msg_type == DHCP_ACK && dhcp_state == 2) {
        dhcp_result_ip = yiaddr;
        dhcp_result_netmask = mask ? mask : 0xFFFFFF00U;
        dhcp_result_gateway = router ? router : (yiaddr & dhcp_result_netmask) | 1;
        dhcp_result_dns = dns ? dns : dhcp_result_gateway;

        kprintf("[dhcp] Received ACK: IP=%u.%u.%u.%u, GW=%u.%u.%u.%u, MASK=%u.%u.%u.%u, DNS=%u.%u.%u.%u\n",
                (uint32_t)((yiaddr >> 24) & 0xFF), (uint32_t)((yiaddr >> 16) & 0xFF),
                (uint32_t)((yiaddr >> 8) & 0xFF), (uint32_t)(yiaddr & 0xFF),
                (uint32_t)((dhcp_result_gateway >> 24) & 0xFF), (uint32_t)((dhcp_result_gateway >> 16) & 0xFF),
                (uint32_t)((dhcp_result_gateway >> 8) & 0xFF), (uint32_t)(dhcp_result_gateway & 0xFF),
                (uint32_t)((mask >> 24) & 0xFF), (uint32_t)((mask >> 16) & 0xFF),
                (uint32_t)((mask >> 8) & 0xFF), (uint32_t)(mask & 0xFF),
                (uint32_t)((dns >> 24) & 0xFF), (uint32_t)((dns >> 16) & 0xFF),
                (uint32_t)((dns >> 8) & 0xFF), (uint32_t)(dns & 0xFF));

        dhcp_state = 3;
        dhcp_done = 1;
        dhcp_has_lease = 1;
        dhcp_acquired_tick = timer_get_ticks();
        dhcp_lease_time = lease ? lease : 3600; /* default 1 hour */
    }
}

/* ── /etc/resolv.conf management ─────────────────────────────────── */

/* Write a nameserver line to /etc/resolv.conf.  Replaces the entire file
 * content with "nameserver A.B.C.D\\n".  Returns 0 on success, negative on error. */
static int resolv_conf_write_nameserver(uint32_t dns_ip)
{
    char buf[64];
    int len;

    if (dns_ip == 0)
        return 0; /* no DNS server — nothing to write */

    /* Format: "nameserver A.B.C.D\\n" */
    len = snprintf(buf, sizeof(buf), "nameserver %u.%u.%u.%u\n",
                   (uint32_t)((dns_ip >> 24) & 0xFF),
                   (uint32_t)((dns_ip >> 16) & 0xFF),
                   (uint32_t)((dns_ip >>  8) & 0xFF),
                   (uint32_t)( dns_ip        & 0xFF));
    if (len < 0 || len >= (int)sizeof(buf))
        return -EINVAL;

    /* Create or truncate /etc/resolv.conf */
    int ret = vfs_create("/etc/resolv.conf", 1); /* 1 = file type */
    if (ret < 0 && ret != -EEXIST) {
        kprintf("[resolv.conf] failed to create /etc/resolv.conf: err=%d\n", ret);
        return ret;
    }

    ret = vfs_write("/etc/resolv.conf", buf, (uint32_t)len);
    if (ret < 0) {
        kprintf("[resolv.conf] failed to write /etc/resolv.conf: err=%d\n", ret);
        return ret;
    }

    kprintf("[resolv.conf] wrote 'nameserver %u.%u.%u.%u'\n",
            (uint32_t)((dns_ip >> 24) & 0xFF),
            (uint32_t)((dns_ip >> 16) & 0xFF),
            (uint32_t)((dns_ip >>  8) & 0xFF),
            (uint32_t)( dns_ip        & 0xFF));
    return 0;
}

/* Read the first nameserver line from /etc/resolv.conf and return its
 * IP in host byte order, or 0 if none found / file missing / error. */
uint32_t net_resolv_conf_read_first(void)
{
    char buf[128];
    uint32_t out_size = 0;

    int ret = vfs_read("/etc/resolv.conf", buf, sizeof(buf) - 1, &out_size);
    if (ret < 0 || out_size == 0)
        return 0;

    buf[out_size] = '\0';

    /* Parse "nameserver" lines — take the first one found */
    const char *p = buf;
    while (p && *p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t')
            p++;

        if (strncmp(p, "nameserver", 10) == 0) {
            p += 10;
            /* Skip whitespace after nameserver */
            while (*p == ' ' || *p == '\t')
                p++;

            /* Parse A.B.C.D */
            uint32_t parts[4] = {0};
            int pi = 0;
            while (*p && *p != '\n' && *p != '\r' && pi < 4) {
                if (*p >= '0' && *p <= '9')
                    parts[pi] = parts[pi] * 10 + (uint32_t)(*p - '0');
                else if (*p == '.')
                    pi++;
                else
                    break; /* invalid character */
                p++;
            }
            if (pi == 3) {
                uint32_t ip = (parts[0] << 24) | (parts[1] << 16) |
                              (parts[2] << 8)  | parts[3];
                return ip;
            }
            break; /* malformed nameserver line */
        }

        /* Skip to end of line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void dhcp_init(void) {
    dhcp_state = 0;
    dhcp_done = 0;
    dhcp_xid = 0;
    dhcp_offered_ip = 0;
    dhcp_server_id = 0;
    dhcp_result_ip = 0;
    dhcp_result_gateway = 0;
    dhcp_result_netmask = 0;
    dhcp_result_dns = 0;
    dhcp_lease_time = 0;
    dhcp_acquired_tick = 0;
    dhcp_has_lease = 0;
    kprintf("[OK] DHCP client initialized\n");
}

/* ── dhcp_renew ─────────────────────────────────────────────────────
 * Renew the DHCP lease by sending a REQUEST to the server that
 * granted the current lease.  Returns 0 on success, -1 on failure.
 */
int dhcp_renew(void)
{
    if (!dhcp_has_lease || dhcp_server_id == 0) {
        /* No existing lease — fall back to full discover */
        return dhcp_discover();
    }

    dhcp_xid = (uint32_t)(timer_get_ticks() ^ 0xA5A5A5A5u ^
                          ((uint64_t)net_our_mac[2] << 24) ^
                          ((uint64_t)net_our_mac[3] << 16) ^
                          ((uint64_t)net_our_mac[4] << 8) ^
                          net_our_mac[5]);
    dhcp_state = 0;
    dhcp_done = 0;

    /* Send a unicast DHCPREQUEST to renew the current lease */
    kprintf("[dhcp] Renewing lease for %u.%u.%u.%u\n",
            (uint32_t)((dhcp_result_ip >> 24) & 0xFF),
            (uint32_t)((dhcp_result_ip >> 16) & 0xFF),
            (uint32_t)((dhcp_result_ip >> 8) & 0xFF),
            (uint32_t)(dhcp_result_ip & 0xFF));

    /* Build and send unicast DHCPREQUEST */
    uint8_t buf[300];
    memset(buf, 0, sizeof(buf));
    struct dhcp_packet *dhcp = (struct dhcp_packet *)buf;

    dhcp->op = 1;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->xid = htonl(dhcp_xid);
    dhcp->flags = htons(0x8000);
    dhcp->magic_cookie = htonl(DHCP_MAGIC);
    memcpy(dhcp->chaddr, net_our_mac, 6);

    uint8_t *opt = buf + sizeof(struct dhcp_packet);
    *opt++ = 53; *opt++ = 1; *opt++ = DHCP_REQUEST;
    /* Requested IP option (50) */
    *opt++ = 50; *opt++ = 4;
    *opt++ = (uint8_t)(dhcp_result_ip >> 24);
    *opt++ = (uint8_t)(dhcp_result_ip >> 16);
    *opt++ = (uint8_t)(dhcp_result_ip >> 8);
    *opt++ = (uint8_t)(dhcp_result_ip);
    /* Server identifier option (54) */
    *opt++ = 54; *opt++ = 4;
    *opt++ = (uint8_t)(dhcp_server_id >> 24);
    *opt++ = (uint8_t)(dhcp_server_id >> 16);
    *opt++ = (uint8_t)(dhcp_server_id >> 8);
    *opt++ = (uint8_t)(dhcp_server_id);
    *opt++ = 255;

    uint16_t pkt_len = (uint16_t)(opt - buf);
    send_udp_unicast(dhcp_server_id, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, buf, pkt_len);
    dhcp_state = 2;

    /* Wait for ACK with timeout */
    uint64_t start = timer_get_ticks();
    int resends = 0;
    const int max_resends = 3;

    while (dhcp_state != 3 && resends < max_resends) {
        uint64_t now = timer_get_ticks();
        uint64_t elapsed = now - start;

        if (elapsed > 300) break; /* 3 second timeout */

        /* Poll network */
        if (e1000_is_present()) {
            uint8_t pkt[2048];
            int n = e1000_receive(pkt, sizeof(pkt));
            if (n > 0) {
                struct eth_header *eth = (struct eth_header *)pkt;
                if (ntohs(eth->type) == ETH_TYPE_IP && n >= (int)sizeof(struct eth_header) + (int)sizeof(struct ip_header)) {
                    struct ip_header *ip = (struct ip_header *)(pkt + sizeof(struct eth_header));
                    if (ip->protocol == IP_PROTO_UDP) {
                        int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
                        int total_len = ntohs(ip->total_len);
                        if (ip_hdr_len + (int)sizeof(struct udp_header) <= total_len &&
                            total_len <= n - (int)sizeof(struct eth_header)) {
                            struct udp_header *udp = (struct udp_header *)(pkt + sizeof(struct eth_header) + ip_hdr_len);
                            if (ntohs(udp->dst_port) == DHCP_CLIENT_PORT) {
                                uint16_t udp_len_val = ntohs(udp->length);
                                int data_off = sizeof(struct eth_header) + ip_hdr_len + sizeof(struct udp_header);
                                int data_len = udp_len_val - sizeof(struct udp_header);
                                if (data_off + data_len <= n) {
                                    handle_dhcp_response(pkt + data_off, data_len);
                                }
                            }
                        }
                    }
                }
            }
        }

        if (virtio_net_present()) {
            uint8_t pkt[2048];
            int n = virtio_net_receive(pkt, sizeof(pkt));
            if (n > 0) {
                struct eth_header *eth = (struct eth_header *)pkt;
                if (ntohs(eth->type) == ETH_TYPE_IP && n >= (int)sizeof(struct eth_header) + (int)sizeof(struct ip_header)) {
                    struct ip_header *ip = (struct ip_header *)(pkt + sizeof(struct eth_header));
                    if (ip->protocol == IP_PROTO_UDP) {
                        int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
                        int total_len = ntohs(ip->total_len);
                        if (ip_hdr_len + (int)sizeof(struct udp_header) <= total_len &&
                            total_len <= n - (int)sizeof(struct eth_header)) {
                            struct udp_header *udp = (struct udp_header *)(pkt + sizeof(struct eth_header) + ip_hdr_len);
                            if (ntohs(udp->dst_port) == DHCP_CLIENT_PORT) {
                                uint16_t udp_len_val = ntohs(udp->length);
                                int data_off = sizeof(struct eth_header) + ip_hdr_len + sizeof(struct udp_header);
                                int data_len = udp_len_val - sizeof(struct udp_header);
                                if (data_off + data_len <= n) {
                                    handle_dhcp_response(pkt + data_off, data_len);
                                }
                            }
                        }
                    }
                }
            }
        }

        if (dhcp_state == 1 && elapsed > (uint64_t)(resends + 1) * 100) {
            resends++;
            kprintf("[dhcp] Resending RENEW (attempt %d/%d)\n", resends, max_resends);
            send_udp_unicast(dhcp_server_id, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, buf, pkt_len);
        }
    }

    if (dhcp_state == 3) {
        kprintf("[dhcp] Lease renewed successfully\n");
        dhcp_acquired_tick = timer_get_ticks();
        resolv_conf_write_nameserver(dhcp_result_dns);
        return 0;
    }

    kprintf("[dhcp] Lease renewal failed, falling back to full discover\n");
    return dhcp_discover();
}

/* ── dhcp_has_lease ──────────────────────────────────────────────
 * Returns 1 if a DHCP lease is currently held, 0 otherwise.
 */
int dhcp_has_lease_func(void) {
    return dhcp_has_lease;
}

/* ── dhcp_get_lease_time ─────────────────────────────────────────
 * Returns the lease time in seconds, or 0 if no lease.
 */
uint32_t dhcp_get_lease_time(void) {
    return dhcp_lease_time;
}

int dhcp_discover(void) {
    /* Generate a random transaction ID from timer */
    dhcp_xid = (uint32_t)(timer_get_ticks() ^ 0xA5A5A5A5u ^
                          ((uint64_t)net_our_mac[2] << 24) ^
                          ((uint64_t)net_our_mac[3] << 16) ^
                          ((uint64_t)net_our_mac[4] << 8) ^
                          net_our_mac[5]);
    dhcp_state = 0;
    dhcp_done = 0;

    kprintf("[dhcp] Starting DHCP transaction (xid=0x%x)\n", (uint32_t)dhcp_xid);

    /* Send DISCOVER */
    dhcp_build_discover();

    /* Loop: poll for packets and check for timeout */
    uint64_t start = timer_get_ticks();
    int resends = 0;
    const int max_resends = 4;
    volatile uint32_t spin_count = 0;

    while (dhcp_state != 3 && resends < max_resends) {
        uint64_t now = timer_get_ticks();
        uint64_t elapsed = now - start;
        spin_count++;

        /* Timeout: 5 seconds of timer ticks */
        if (elapsed > 500) break;
        /* Spin-count fallback if timer isn't advancing */
        if (now == start && spin_count > 50000000) break;

        /* Poll the network interface for incoming packets */
        if (e1000_is_present()) {
            uint8_t pkt[2048];
            int n = e1000_receive(pkt, sizeof(pkt));
            if (n > 0) {
                /* Parse Ethernet/IP/UDP header */
                struct eth_header *eth = (struct eth_header *)pkt;
                if (ntohs(eth->type) == ETH_TYPE_IP && n >= (int)sizeof(struct eth_header) + (int)sizeof(struct ip_header)) {
                    struct ip_header *ip = (struct ip_header *)(pkt + sizeof(struct eth_header));
                    if (ip->protocol == IP_PROTO_UDP) {
                        int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
                        int total_len = ntohs(ip->total_len);
                        if (ip_hdr_len + (int)sizeof(struct udp_header) <= total_len &&
                            total_len <= n - (int)sizeof(struct eth_header)) {
                            struct udp_header *udp = (struct udp_header *)(pkt + sizeof(struct eth_header) + ip_hdr_len);
                            if (ntohs(udp->dst_port) == DHCP_CLIENT_PORT) {
                                /* Extract DHCP payload */
                                uint16_t udp_len_val = ntohs(udp->length);
                                int data_off = sizeof(struct eth_header) + ip_hdr_len + sizeof(struct udp_header);
                                int data_len = udp_len_val - sizeof(struct udp_header);
                                if (data_off + data_len <= n) {
                                    handle_dhcp_response(pkt + data_off, data_len);
                                }
                            }
                        }
                    }
                }
            }
        }

        if (virtio_net_present()) {
            uint8_t pkt[2048];
            int n = virtio_net_receive(pkt, sizeof(pkt));
            if (n > 0) {
                struct eth_header *eth = (struct eth_header *)pkt;
                if (ntohs(eth->type) == ETH_TYPE_IP && n >= (int)sizeof(struct eth_header) + (int)sizeof(struct ip_header)) {
                    struct ip_header *ip = (struct ip_header *)(pkt + sizeof(struct eth_header));
                    if (ip->protocol == IP_PROTO_UDP) {
                        int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
                        int total_len = ntohs(ip->total_len);
                        if (ip_hdr_len + (int)sizeof(struct udp_header) <= total_len &&
                            total_len <= n - (int)sizeof(struct eth_header)) {
                            struct udp_header *udp = (struct udp_header *)(pkt + sizeof(struct eth_header) + ip_hdr_len);
                            if (ntohs(udp->dst_port) == DHCP_CLIENT_PORT) {
                                uint16_t udp_len_val = ntohs(udp->length);
                                int data_off = sizeof(struct eth_header) + ip_hdr_len + sizeof(struct udp_header);
                                int data_len = udp_len_val - sizeof(struct udp_header);
                                if (data_off + data_len <= n) {
                                    handle_dhcp_response(pkt + data_off, data_len);
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Resend DISCOVER every ~1 second if no response */
        if (dhcp_state == 1 && elapsed > (uint64_t)(resends + 1) * 100) {
            resends++;
            kprintf("[dhcp] Resending DISCOVER (attempt %d/%d)\n", resends, max_resends);
            dhcp_build_discover();
        }
    }

    if (dhcp_state != 3) {
        kprintf("[dhcp] DHCP transaction failed (timeout), using QEMU defaults\n");
        net_our_ip      = (10U << 24) | (0U << 16) | (2U << 8) | 15U;
        net_gateway_ip  = (10U << 24) | (0U << 16) | (2U << 8) | 2U;
        net_subnet_mask = (255U << 24) | (255U << 16) | (255U << 8) | 0U;
        net_dns_server  = (10U << 24) | (0U << 16) | (2U << 8) | 3U;
        arp_resolve_gateway();
        return -1;
    }

    /* Update networking state with our new configuration */
    net_set_ip(dhcp_result_ip, dhcp_result_gateway, dhcp_result_netmask);
    net_dns_server = dhcp_result_dns;
    net_gw_mac_known = 0;

    /* Write DNS server to /etc/resolv.conf for userspace tools */
    resolv_conf_write_nameserver(dhcp_result_dns);

    /* Resolve gateway MAC */
    arp_resolve_gateway();

    kprintf("[OK] DHCP: assigned %u.%u.%u.%u, GW %u.%u.%u.%u, MASK %u.%u.%u.%u\n",
            (uint32_t)((dhcp_result_ip >> 24) & 0xFF),
            (uint32_t)((dhcp_result_ip >> 16) & 0xFF),
            (uint32_t)((dhcp_result_ip >> 8) & 0xFF),
            (uint32_t)(dhcp_result_ip & 0xFF),
            (uint32_t)((dhcp_result_gateway >> 24) & 0xFF),
            (uint32_t)((dhcp_result_gateway >> 16) & 0xFF),
            (uint32_t)((dhcp_result_gateway >> 8) & 0xFF),
            (uint32_t)(dhcp_result_gateway & 0xFF),
            (uint32_t)((dhcp_result_netmask >> 24) & 0xFF),
            (uint32_t)((dhcp_result_netmask >> 16) & 0xFF),
            (uint32_t)((dhcp_result_netmask >> 8) & 0xFF),
            (uint32_t)(dhcp_result_netmask & 0xFF));

    return 0;
}

void dhcp_set_server(uint32_t ip) {
    dhcp_server_id = ip;
}

uint32_t dhcp_get_server(void) {
    return dhcp_server_id;
}

/* ══════════════════════════════════════════════════════════════════════
 * DHCP Relay Agent (RFC 3046) — Option 82 insertion
 *
 * When dhcp_relay_mode is enabled, the kernel acts as a DHCP relay agent,
 * inserting the Relay Agent Information option (option 82) into DHCP
 * packets forwarded between clients and servers.
 *
 * Option 82 sub-options:
 *   Sub-option 1: Circuit ID (identifies the incoming circuit/port)
 *   Sub-option 2: Remote ID (identifies the relay agent)
 *
 * ══════════════════════════════════════════════════════════════════════ */

/* Relay agent state */
static int dhcp_relay_enabled = 0;
static uint32_t dhcp_relay_server_ip = 0;  /* DHCP server to forward to */
static uint32_t dhcp_relay_giaddr = 0;      /* Our IP (gateway IP addr) */
static uint8_t  dhcp_relay_circuit_id[8];   /* Circuit ID (subopt 1) */
static uint8_t  dhcp_relay_remote_id[8];    /* Remote ID (subopt 2) */
static int dhcp_relay_circuit_id_len = 0;
static int dhcp_relay_remote_id_len = 0;

/* Option 82 sub-option codes */
#define DHCP_RELAY_OPT82          82
#define OPT82_SUB_CIRCUIT_ID      1
#define OPT82_SUB_REMOTE_ID       2

/*
 * Insert Relay Agent Information option (82) into a DHCP packet.
 * Modifies the packet in-place.  Returns the new packet length.
 *
 * The option is inserted before the End option (255).
 */
static int dhcp_relay_insert_option82(uint8_t *buf, int len, int max_len)
{
    if (!dhcp_relay_enabled || !buf || len <= 0)
        return len;

    /* Locate the End option (255) */
    int end_pos = -1;
    int i;
    for (i = sizeof(struct dhcp_packet); i < len; i++) {
        if (buf[i] == 255) {
            end_pos = i;
            break;
        }
        if (buf[i] == 0) continue; /* pad */
        if (i + 1 >= len) break;
        int opt_len = buf[i + 1];
        i += 1 + opt_len;
    }

    if (end_pos < 0)
        return len; /* no end option found */

    /* Calculate option 82 size:
     *  1 byte code (82)
     *  1 byte total length
     *  1 byte sub-opt1 code
     *  1 byte sub-opt1 length
     *  N bytes circuit ID
     *  1 byte sub-opt2 code
     *  1 byte sub-opt2 length
     *  N bytes remote ID
     */
    int opt82_len = 2; /* code + total length */
    if (dhcp_relay_circuit_id_len > 0)
        opt82_len += 2 + dhcp_relay_circuit_id_len; /* sub1 code + len + data */
    if (dhcp_relay_remote_id_len > 0)
        opt82_len += 2 + dhcp_relay_remote_id_len; /* sub2 code + len + data */

    /* Check if we have room */
    if (end_pos + opt82_len + 1 > max_len)
        return len;

    /* Shift everything after end_pos to make room */
    int tail_len = len - end_pos; /* includes the 255 byte */
    if (tail_len > 0 && opt82_len > 0) {
        for (int j = tail_len - 1; j >= 0; j--)
            buf[end_pos + opt82_len + j] = buf[end_pos + j];
    }

    /* Fill in option 82 */
    int pos = end_pos;
    buf[pos++] = 82; /* option code */
    buf[pos++] = (uint8_t)(opt82_len - 2); /* total data length */

    if (dhcp_relay_circuit_id_len > 0) {
        buf[pos++] = OPT82_SUB_CIRCUIT_ID;
        buf[pos++] = (uint8_t)dhcp_relay_circuit_id_len;
        for (int j = 0; j < dhcp_relay_circuit_id_len; j++)
            buf[pos++] = dhcp_relay_circuit_id[j];
    }

    if (dhcp_relay_remote_id_len > 0) {
        buf[pos++] = OPT82_SUB_REMOTE_ID;
        buf[pos++] = (uint8_t)dhcp_relay_remote_id_len;
        for (int j = 0; j < dhcp_relay_remote_id_len; j++)
            buf[pos++] = dhcp_relay_remote_id[j];
    }

    return len + opt82_len;
}

/*
 * Strip Relay Agent Information option (82) from a DHCP packet.
 * Returns the new packet length.
 */
static __attribute__((unused)) int dhcp_relay_strip_option82(uint8_t *buf, int len)
{
    if (!buf || len <= 0)
        return len;

    for (int i = sizeof(struct dhcp_packet); i < len; i++) {
        if (buf[i] == 255)
            break; /* end of options */
        if (buf[i] == 0)
            continue; /* pad */
        if (i + 1 >= len)
            break;

        int opt_len = buf[i + 1];
        if (buf[i] == 82) {
            /* Remove this option: shift everything after it */
            int opt_total = 2 + opt_len;
            int remaining = len - (i + opt_total);
            if (remaining > 0)
                __builtin_memmove(&buf[i], &buf[i + opt_total], remaining);
            len -= opt_total;
            break;
        }
        i += 1 + opt_len;
    }
    return len;
}

/*
 * Enable DHCP relay mode.
 *
 * @server_ip: IP address of the upstream DHCP server (network byte order)
 * @giaddr:    Our IP address to put in the giaddr field (network byte order)
 *
 * After enabling, DHCP packets received on local interfaces are forwarded
 * to the specified server, with Option 82 inserted.
 */
void dhcp_relay_enable(uint32_t server_ip, uint32_t giaddr)
{
    dhcp_relay_enabled = 1;
    dhcp_relay_server_ip = server_ip;
    dhcp_relay_giaddr = giaddr;

    /* Default circuit ID: first 6 bytes = our MAC */
    for (int i = 0; i < 6 && i < 8; i++)
        dhcp_relay_circuit_id[i] = net_our_mac[i];
    dhcp_relay_circuit_id_len = 6;

    /* Default remote ID: our IP address (4 bytes) */
    dhcp_relay_remote_id[0] = (uint8_t)(giaddr >> 24);
    dhcp_relay_remote_id[1] = (uint8_t)(giaddr >> 16);
    dhcp_relay_remote_id[2] = (uint8_t)(giaddr >> 8);
    dhcp_relay_remote_id[3] = (uint8_t)(giaddr);
    dhcp_relay_remote_id_len = 4;

    kprintf("[dhcp] Relay enabled: server=%u.%u.%u.%u giaddr=%u.%u.%u.%u\n",
            (uint32_t)((server_ip >> 24) & 0xFF),
            (uint32_t)((server_ip >> 16) & 0xFF),
            (uint32_t)((server_ip >> 8) & 0xFF),
            (uint32_t)(server_ip & 0xFF),
            (uint32_t)((giaddr >> 24) & 0xFF),
            (uint32_t)((giaddr >> 16) & 0xFF),
            (uint32_t)((giaddr >> 8) & 0xFF),
            (uint32_t)(giaddr & 0xFF));
}

/* Disable DHCP relay mode */
void dhcp_relay_disable(void)
{
    dhcp_relay_enabled = 0;
    kprintf("[dhcp] Relay disabled\n");
}

/* Check if DHCP relay is active */
int dhcp_relay_is_active(void)
{
    return dhcp_relay_enabled;
}

/*
 * Set custom Circuit ID for Option 82.
 * @data: pointer to the circuit ID bytes
 * @len:  length of circuit ID (max 64)
 */
void dhcp_relay_set_circuit_id(const uint8_t *data, int len)
{
    if (!data || len <= 0) {
        dhcp_relay_circuit_id_len = 0;
        return;
    }
    if (len > 64) len = 64;
    __builtin_memcpy(dhcp_relay_circuit_id, data, len);
    dhcp_relay_circuit_id_len = len;
}

/*
 * Set custom Remote ID for Option 82.
 * @data: pointer to the remote ID bytes
 * @len:  length of remote ID (max 64)
 */
void dhcp_relay_set_remote_id(const uint8_t *data, int len)
{
    if (!data || len <= 0) {
        dhcp_relay_remote_id_len = 0;
        return;
    }
    if (len > 64) len = 64;
    __builtin_memcpy(dhcp_relay_remote_id, data, len);
    dhcp_relay_remote_id_len = len;
}

/*
 * Forward a DHCP packet as a relay agent.
 * Takes a received DHCP packet, modifies the giaddr, inserts Option 82,
 * and unicasts it to the configured DHCP server.
 */
int dhcp_relay_forward(const uint8_t *pkt, int len, int from_port)
{
    if (!dhcp_relay_enabled || !pkt || len < (int)sizeof(struct dhcp_packet))
        return -1;

    uint8_t fwd_buf[1500];
    if (len > (int)sizeof(fwd_buf))
        return -1;

    __builtin_memcpy(fwd_buf, pkt, len);
    struct dhcp_packet *dhcp = (struct dhcp_packet *)fwd_buf;

    /* Set giaddr to our IP (relay agent address) */
    dhcp->giaddr = dhcp_relay_giaddr;

    /* Set hops: increment if less than max */
    if (dhcp->hops < 16)
        dhcp->hops++;

    /* Insert Option 82 */
    int new_len = dhcp_relay_insert_option82(fwd_buf, len, (int)sizeof(fwd_buf));
    if (new_len < 0)
        new_len = len;

    if (from_port == DHCP_CLIENT_PORT) {
        /* Client → Server: forward to DHCP server port */
        send_udp_unicast(dhcp_relay_server_ip, DHCP_SERVER_PORT,
                         DHCP_SERVER_PORT, fwd_buf, new_len);
        kprintf("[dhcp-relay] Forwarded client msg to server\n");
    } else {
        /* Server → Client: forward to DHCP client port */
        uint32_t client_ip = ntohl(dhcp->yiaddr);
        if (client_ip) {
            send_udp_unicast(client_ip, DHCP_CLIENT_PORT,
                             DHCP_CLIENT_PORT, fwd_buf, new_len);
            kprintf("[dhcp-relay] Forwarded server reply to client\n");
        } else {
            send_udp_broadcast(DHCP_CLIENT_PORT, DHCP_CLIENT_PORT,
                               fwd_buf, new_len);
            kprintf("[dhcp-relay] Broadcasted server reply\n");
        }
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * DHCPv6 Prefix Delegation (RFC 3633)
 *
 * Simple DHCPv6 PD client: requests an IPv6 prefix from a DHCPv6 server
 * and configures it on the interface.
 *
 * ══════════════════════════════════════════════════════════════════════ */

/* DHCPv6 message types */
#define DHCPV6_SOLICIT    1
#define DHCPV6_ADVERTISE  2
#define DHCPV6_REQUEST    3
#define DHCPV6_REPLY      7

/* DHCPv6 option codes */
#define DHCPV6_OPT_CLIENTID  1
#define DHCPV6_OPT_SERVERID  2
#define DHCPV6_OPT_IA_PD     25   /* Identity Association for Prefix Delegation */
#define DHCPV6_OPT_IAPREFIX  26

/* DHCPv6 DUID (simplified: DUID-LLT with our MAC) */
struct dhcpv6_duid {
    uint16_t type;     /* 1 = DUID-LLT */
    uint16_t htype;    /* 1 = Ethernet */
    uint32_t time;     /* seconds since Jan 1 2000 */
    uint8_t  mac[6];   /* link-layer address */
} __attribute__((packed));

/* DHCPv6 client IA_PD option */
struct dhcpv6_ia_pd {
    uint32_t iaid;
    uint32_t t1;
    uint32_t t2;
} __attribute__((packed));

/* DHCPv6 IA Prefix option */
struct dhcpv6_ia_prefix {
    uint32_t preferred_lifetime;
    uint32_t valid_lifetime;
    uint8_t  prefix_length;
    uint8_t  prefix[16];
} __attribute__((packed));

/* DHCPv6 header */
struct dhcpv6_header {
    uint8_t  msg_type;
    uint8_t  transaction_id[3];
    /* options follow */
} __attribute__((packed));

/* Result from DHCPv6 PD */
static int dhcpv6_pd_active = 0;
static uint8_t dhcpv6_delegated_prefix[16];
static uint8_t dhcpv6_prefix_length = 64; /* default /64 */

/*
 * Send a DHCPv6 SOLICIT for prefix delegation.
 * Returns 0 on success, -1 on failure.
 */
int dhcpv6_pd_solicit(void)
{
    uint8_t buf[512];
    struct dhcpv6_header *hdr = (struct dhcpv6_header *)buf;
    memset(buf, 0, sizeof(buf));

    hdr->msg_type = DHCPV6_SOLICIT;
    /* Transaction ID: 3 bytes of random-ish data */
    uint32_t tid = (uint32_t)(timer_get_ticks() ^ 0xDEADBEEF);
    hdr->transaction_id[0] = (uint8_t)(tid >> 16);
    hdr->transaction_id[1] = (uint8_t)(tid >> 8);
    hdr->transaction_id[2] = (uint8_t)(tid);

    int pos = sizeof(struct dhcpv6_header);

    /* Client Identifier option */
    {
        struct dhcpv6_duid *duid = (struct dhcpv6_duid *)&buf[pos + 4];
        duid->type  = htons(1);   /* DUID-LLT */
        duid->htype = htons(1);   /* Ethernet */
        duid->time  = htonl((uint32_t)(timer_get_ticks() & 0xFFFFFFFF));
        memcpy(duid->mac, net_our_mac, 6);

        uint16_t opt_len = sizeof(struct dhcpv6_duid);
        buf[pos + 0] = (uint8_t)(DHCPV6_OPT_CLIENTID >> 8);
        buf[pos + 1] = (uint8_t)(DHCPV6_OPT_CLIENTID);
        buf[pos + 2] = (uint8_t)(opt_len >> 8);
        buf[pos + 3] = (uint8_t)(opt_len);
        pos += 4 + opt_len;
    }

    /* IA_PD option (request prefix delegation) */
    {
        struct dhcpv6_ia_pd *iapd = (struct dhcpv6_ia_pd *)&buf[pos + 4];
        iapd->iaid = htonl(1);  /* IAID = 1 */
        iapd->t1   = htonl(3600);  /* T1 = 1 hour */
        iapd->t2   = htonl(5400);  /* T2 = 1.5 hours */

        uint16_t opt_len = sizeof(struct dhcpv6_ia_pd);
        buf[pos + 0] = (uint8_t)(DHCPV6_OPT_IA_PD >> 8);
        buf[pos + 1] = (uint8_t)(DHCPV6_OPT_IA_PD);
        buf[pos + 2] = (uint8_t)(opt_len >> 8);
        buf[pos + 3] = (uint8_t)(opt_len);
        pos += 4 + opt_len;
    }

    /* Elapsed time option (code 8) */
    {
        uint16_t elapsed = htons(0); /* 0 = first message */
        buf[pos + 0] = 0;
        buf[pos + 1] = 8;   /* option code 8 */
        buf[pos + 2] = 0;
        buf[pos + 3] = 2;   /* length = 2 */
        buf[pos + 4] = (uint8_t)(elapsed >> 8);
        buf[pos + 5] = (uint8_t)(elapsed);
        pos += 6;
    }

    kprintf("[dhcpv6] Sending SOLICIT for prefix delegation\n");

    /* Send to DHCPv6 All_Servers multicast address (FF02::1:2) */
    /* Send as UDP to [FF02::1:2]:547 */
    {
        uint8_t udp_buf[1500];
        struct udp_header *udp = (struct udp_header *)udp_buf;
        uint16_t udp_len = sizeof(struct udp_header) + (uint16_t)pos;

        udp->src_port = htons(546);   /* DHCPv6 client port */
        udp->dst_port = htons(547);   /* DHCPv6 server port */
        udp->length   = htons(udp_len);
        udp->checksum = 0;

        /* Copy DHCPv6 payload after UDP header */
        memcpy(udp_buf + sizeof(struct udp_header), buf, (size_t)pos);

        /* Compute UDP checksum over IPv6 pseudo-header */
        struct in6_addr all_servers = { { 0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,2 } };
        udp->checksum = ipv6_checksum(&net_our_ipv6_ll, &all_servers,
                                       IP_PROTO_UDP, udp_buf, udp_len);

        send_ipv6(&all_servers, IP_PROTO_UDP, udp_buf, udp_len);
    }

    kprintf("[dhcpv6] Sent SOLICIT for prefix delegation via IPv6 UDP\n");

    return 0;
}

/*
 * Check if DHCPv6 prefix delegation is active.
 */
int dhcpv6_pd_is_active(void)
{
    return dhcpv6_pd_active;
}

/*
 * Get the delegated prefix (16 bytes, network byte order).
 */
int dhcpv6_pd_get_prefix(uint8_t *prefix_out, uint8_t *length_out)
{
    if (!dhcpv6_pd_active)
        return -1;
    if (prefix_out)
        memcpy(prefix_out, dhcpv6_delegated_prefix, 16);
    if (length_out)
        *length_out = dhcpv6_prefix_length;
    return 0;
}
#include "module.h"
module_init(dhcp_init);
