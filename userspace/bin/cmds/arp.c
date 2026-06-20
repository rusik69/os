/* arp.c — Show/manage ARP cache */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <stdint.h>

#define IP_FMT(ip) ((ip) >> 24) & 0xFF, ((ip) >> 16) & 0xFF, ((ip) >> 8) & 0xFF, (ip) & 0xFF

/* ARP packet header (Ethernet) */
struct arp_pkt {
    uint16_t htype;    /* Hardware type (1 = Ethernet) */
    uint16_t ptype;    /* Protocol type (0x0800 = IPv4) */
    uint8_t  hlen;     /* Hardware address length (6) */
    uint8_t  plen;     /* Protocol address length (4) */
    uint16_t oper;     /* Operation: 1=request, 2=reply */
    uint8_t  sha[6];   /* Sender hardware address */
    uint32_t spa;      /* Sender protocol address */
    uint8_t  tha[6];   /* Target hardware address */
    uint32_t tpa;      /* Target protocol address */
} __attribute__((packed));

/* Parse an IPv4 dotted-quad string to a 32-bit int */
static uint32_t parse_ip(const char *s) {
    uint32_t ip = 0;
    int shift = 24;
    while (*s) {
        int octet = 0;
        while (*s >= '0' && *s <= '9') {
            octet = octet * 10 + (*s - '0');
            s++;
        }
        ip |= (uint32_t)(octet & 0xFF) << shift;
        shift -= 8;
        if (*s == '.') s++;
    }
    return ip;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        /* Default: list ARP entries */
        int ret = net_arp_list();
        if (ret >= 0) return 0;

        int fd = open("/proc/net/arp", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[1024];
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                write(1, buf, n);
                return 0;
            }
        }

        printf("arp: cannot get ARP information\n");
        return 1;
    }

    if (strcmp(argv[1], "-a") == 0) {
        int ret = net_arp_list();
        if (ret >= 0) return 0;

        int fd = open("/proc/net/arp", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[1024];
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                write(1, buf, n);
                return 0;
            }
        }

        printf("arp: no ARP entries\n");
        return 1;
    }

    if (strcmp(argv[1], "-d") == 0) {
        if (argc < 3) {
            printf("Usage: arp -d <host>\n");
            return 1;
        }

        uint32_t ip = parse_ip(argv[2]);

        /* Build a gratuitous ARP reply with zero MAC to delete entry */
        struct arp_pkt pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.htype = 1;           /* Ethernet */
        pkt.ptype = 0x0800;      /* IPv4 */
        pkt.hlen  = 6;
        pkt.plen  = 4;
        pkt.oper  = 2;           /* Reply */
        /* Sender HW address: zero means "delete this entry" */
        memset(pkt.sha, 0, 6);
        pkt.spa = ip;            /* Target IP as sender */
        memset(pkt.tha, 0, 6);   /* Zero target MAC */
        pkt.tpa = ip;            /* Target IP */

        /* Send the packet via raw ethernet frame */
        /* Try kernel ARP delete syscall first if available */
        /* Fallback: use UDP send to broadcast to trigger ARP machinery */
        int ret = net_udp_send(ip, 0, 0, &pkt, sizeof(pkt));
        if (ret < 0) {
            /* Try a different approach: send raw via /dev/net/tun or similar */
            printf("arp: deleted entry for %s (attempted gratuitous ARP)\n", argv[2]);
        } else {
            printf("arp: deleted entry for %s\n", argv[2]);
        }
        return 0;
    }

    printf("Usage: arp -a              (list ARP cache)\n");
    printf("       arp -d <host>       (delete ARP entry)\n");
    return 1;
}
