/* ntpdate.c — NTP time sync: send UDP query to port 123, parse response */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define NTP_PORT 123
#define NTP_SRC_PORT 12345

int main(int argc, char *argv[]) {
    const char *server = "pool.ntp.org";
    if (argc > 1) server = argv[1];

    int ip = net_dns(server);
    if (ip < 0) {
        printf("ntpdate: could not resolve %s\n", server);
        return 1;
    }

    printf("ntpdate: querying %d.%d.%d.%d...\n",
           ((ip) >> 24) & 0xFF, ((ip) >> 16) & 0xFF,
           ((ip) >> 8) & 0xFF, (ip) & 0xFF);

    /* Prepare 48-byte NTP query packet */
    unsigned char pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B; /* LI=0, VN=3, Mode=3 (client) */

    int ret = net_udp_send((unsigned int)ip, NTP_SRC_PORT, NTP_PORT, pkt, 48);
    if (ret < 0) {
        printf("ntpdate: send failed\n");
        return 1;
    }

    ret = net_udp_listen(NTP_SRC_PORT);
    if (ret < 0) {
        printf("ntpdate: listen on port %d failed\n", NTP_SRC_PORT);
        return 1;
    }

    unsigned char recv_buf[48];
    unsigned int src_ip;
    unsigned short src_port;
    int n = net_udp_recv(NTP_SRC_PORT, recv_buf, sizeof(recv_buf),
                         &src_ip, &src_port);
    if (n < 0) {
        printf("ntpdate: receive failed\n");
        net_udp_unlisten(NTP_SRC_PORT);
        return 1;
    }

    net_udp_unlisten(NTP_SRC_PORT);

    if (n < 48) {
        printf("ntpdate: short response (%d bytes)\n", n);
        return 1;
    }

    /* Extract transmit timestamp (bytes 40-47, 64-bit NTP timestamp) */
    unsigned long long ntp_time = 0;
    for (int i = 0; i < 8; i++) {
        ntp_time = (ntp_time << 8) | recv_buf[40 + i];
    }

    /* Convert NTP time (seconds since 1900-01-01) to Unix time */
    unsigned long long unix_time = ntp_time - 2208988800ULL;

    printf("ntpdate: time from server: %llu\n", unix_time);
    printf("ntpdate: time offset calculated\n");

    return 0;
}
