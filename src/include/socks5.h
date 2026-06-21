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

/* SOCKS5 BIND support — bind to a port on the proxy and accept connections.
 * Returns a socket fd on success, sets *bind_port to the proxy-assigned port. */
int socks5_bind(const struct socks5_server *srv,
                const char *dest_host, uint16_t dest_port,
                uint16_t *bind_port);

/* SOCKS5 UDP ASSOCIATE support — create UDP association through proxy.
 * Returns a socket fd on success, sets *assoc_port to the proxy-assigned port. */
int socks5_udp_associate(const struct socks5_server *srv,
                          const char *dest_host, uint16_t dest_port,
                          uint16_t *assoc_port);

void socks5_init(void);

#endif /* SOCKS5_H */
