/* dns.c — DNS lookup using net_dns syscall */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

#define IP_FMT(ip) ((ip) >> 24) & 0xFF, ((ip) >> 16) & 0xFF, ((ip) >> 8) & 0xFF, (ip) & 0xFF

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: dns <hostname>\n");
        return 1;
    }

    const char *hostname = argv[1];
    int result = net_dns(hostname);

    if (result < 0) {
        printf("dns: could not resolve '%s'\n", hostname);
        return 1;
    }

    unsigned int ip = (unsigned int)result;
    printf("%s has address %d.%d.%d.%d\n", hostname, IP_FMT(ip));
    return 0;
}
