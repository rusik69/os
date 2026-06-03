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
    ip->id = htons(net_ip_id_counter++);
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
    kprintf("[OK] DHCP client initialized\n");
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

    while (dhcp_state != 3 && resends < max_resends) {
        uint64_t now = timer_get_ticks();
        uint64_t elapsed = now - start;

        /* Check for timeout (5 seconds) */
        if (elapsed > 500) break;

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
        kprintf("[dhcp] DHCP transaction failed (timeout)\n");
        return -1;
    }

    /* Update networking state with our new configuration */
    net_set_ip(dhcp_result_ip, dhcp_result_gateway, dhcp_result_netmask);

    /* Write DNS server to /etc/resolv.conf for userspace tools */
    resolv_conf_write_nameserver(dhcp_result_dns);

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
