/* udpsend.c — Send UDP message to host:port with DNS resolution */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: udpsend <host> <port> <message>\n");
        return 1;
    }

    int ip = net_dns(argv[1]);
    if (ip < 0) {
        printf("udpsend: could not resolve %s\n", argv[1]);
        return 1;
    }

    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        printf("udpsend: invalid port %d\n", port);
        return 1;
    }

    int len = strlen(argv[3]);
    int ret = net_udp_send((unsigned int)ip, 12345, (unsigned short)port, argv[3], len);
    if (ret < 0) {
        printf("udpsend: send failed\n");
        return 1;
    }

    printf("udpsend: sent %d bytes to %s:%d\n", len, argv[1], port);
    return 0;
}
