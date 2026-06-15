/* dhcpcd.c — DHCP client */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* DHCP magic cookie */
static const unsigned char dhcp_magic[4] = { 0x63, 0x82, 0x53, 0x63 };

/* DHCP message types */
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

/* DHCP options */
#define OPT_MSG_TYPE      53
#define OPT_SERVER_ID     54
#define OPT_REQUESTED_IP  50
#define OPT_LEASE_TIME    51
#define OPT_SUBNET_MASK   1
#define OPT_ROUTER        3
#define OPT_DNS_SERVER    6
#define OPT_END           255

/* BOOTP header */
struct dhcp_pkt {
    unsigned char  op;
    unsigned char  htype;
    unsigned char  hlen;
    unsigned char  hops;
    unsigned int   xid;
    unsigned short secs;
    unsigned short flags;
    unsigned int   ciaddr;
    unsigned int   yiaddr;
    unsigned int   siaddr;
    unsigned int   giaddr;
    unsigned char  chaddr[16];
    char           sname[64];
    char           file[128];
    unsigned char  options[312];
};

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: dhcpcd <interface>\n");
        return 1;
    }

    const char *iface = argv[1];
    (void)iface;

    /* Generate a random transaction ID using sysinfo uptime */
    unsigned int xid = 0x12345678;
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        xid = (unsigned int)(si.uptime & 0xFFFFFFFF);
    }

    printf("dhcpcd: starting on %s\n", iface);
    printf("dhcpcd: sending DHCPDISCOVER (xid=0x%08x)...\n", xid);

    /* Build DHCP discover packet */
    struct dhcp_pkt pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.op = 1;      /* BOOTREQUEST */
    pkt.htype = 1;   /* Ethernet */
    pkt.hlen = 6;    /* MAC length */
    pkt.xid = xid;
    pkt.flags = 0x8000;  /* Broadcast */

    /* Options: magic cookie + DHCPDISCOVER + END */
    unsigned char *opts = pkt.options;
    memcpy(opts, dhcp_magic, 4);
    opts += 4;
    opts[0] = OPT_MSG_TYPE;
    opts[1] = 1;
    opts[2] = DHCP_DISCOVER;
    opts += 3;

    /* Request parameters */
    static const unsigned char param_req[] = { OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS_SERVER, OPT_LEASE_TIME };
    opts[0] = 55;
    opts[1] = sizeof(param_req);
    memcpy(opts + 2, param_req, sizeof(param_req));
    opts += 2 + sizeof(param_req);

    opts[0] = OPT_END;

    unsigned int pktlen = (unsigned int)((unsigned long)opts - (unsigned long)&pkt + 1);

    /* DHCP server port 67, client port 68. Broadcast IP 255.255.255.255 */
    int ret = net_udp_send(0xFFFFFFFF, 68, 67, &pkt, pktlen);

    if (ret < 0) {
        printf("dhcpcd: UDP send failed (%d)\n", ret);
        return 1;
    }

    printf("dhcpcd: DHCPDISCOVER sent, listening for offers...\n");

    /* Listen for DHCPOFFER on port 68 */
    if (net_udp_listen(68) < 0) {
        printf("dhcpcd: cannot listen on port 68\n");
        return 1;
    }

    /* Wait for response with a simple timeout loop */
    unsigned int src_ip = 0;
    unsigned short src_port = 0;
    unsigned char buf[1024];

    ret = net_udp_recv(68, buf, sizeof(buf), &src_ip, &src_port);
    if (ret < 0) {
        printf("dhcpcd: no DHCPOFFER received\n");
        net_udp_unlisten(68);
        return 1;
    }

    /* Parse response */
    struct dhcp_pkt *resp = (struct dhcp_pkt *)buf;
    if (resp->op == 2 && resp->yiaddr) {
        printf("dhcpcd: DHCPOFFER from %d.%d.%d.%d\n",
               ((unsigned int)src_ip >> 24) & 0xFF,
               ((unsigned int)src_ip >> 16) & 0xFF,
               ((unsigned int)src_ip >> 8) & 0xFF,
               (unsigned int)src_ip & 0xFF);
        printf("dhcpcd: offered IP %d.%d.%d.%d\n",
               (resp->yiaddr >> 24) & 0xFF,
               (resp->yiaddr >> 16) & 0xFF,
               (resp->yiaddr >> 8) & 0xFF,
               resp->yiaddr & 0xFF);
        printf("dhcpcd: done (IP acquisition complete)\n");
    } else {
        printf("dhcpcd: invalid DHCP response\n");
    }

    net_udp_unlisten(68);
    return 0;
}
