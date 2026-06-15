#ifndef SOCKS5_H
#define SOCKS5_H

#include "types.h"

struct socks5_server {
    char     host[256];
    uint16_t port;
    char     username[64];
    char     password[64];
};

/* Connect to a SOCKS5 proxy and establish a tunnel to dest_host:dest_port.
 * Returns a socket fd (int) on success, negative errno on failure. */
int socks5_connect(const struct socks5_server *srv,
                   const char *dest_host, uint16_t dest_port);

void socks5_init(void);

#endif /* SOCKS5_H */
