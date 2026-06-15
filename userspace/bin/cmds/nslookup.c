/* nslookup.c — DNS lookup tool */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

#define IP_FMT(ip) ((ip) >> 24) & 0xFF, ((ip) >> 16) & 0xFF, ((ip) >> 8) & 0xFF, (ip) & 0xFF

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: nslookup <hostname>\n");
        return 1;
    }

    const char *hostname = argv[1];
    printf("Server:         (kernel resolver)\n");
    printf("Address:        (built-in)\n\n");

    int result = net_dns(hostname);
    if (result < 0) {
        printf("*** nslookup: can't resolve '%s': Name or service not known\n", hostname);
        return 1;
    }

    unsigned int ip = (unsigned int)result;
    printf("Name:           %s\n", hostname);
    printf("Address:        %d.%d.%d.%d\n", IP_FMT(ip));
    return 0;
}
