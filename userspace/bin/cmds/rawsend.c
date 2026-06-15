/* rawsend.c — Send UDP message to IP:port (no DNS resolution) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static unsigned int parse_ip(const char *s) {
    unsigned int ip = 0;
    int shift = 24;
    while (*s) {
        unsigned int val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (unsigned int)(*s - '0');
            s++;
        }
        ip |= (val << shift);
        shift -= 8;
        if (*s == '.') s++;
        else break;
    }
    return ip;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: rawsend <host> <port> <message>\n");
        return 1;
    }

    unsigned int ip = parse_ip(argv[1]);
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        printf("rawsend: invalid port %d\n", port);
        return 1;
    }

    int len = strlen(argv[3]);
    int ret = net_udp_send(ip, 12345, (unsigned short)port, argv[3], len);
    if (ret < 0) {
        printf("rawsend: send failed\n");
        return 1;
    }

    printf("rawsend: sent %d bytes to %s:%d\n", len, argv[1], port);
    return 0;
}
