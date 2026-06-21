/*
 * socks5.c — SOCKS5 proxy client implementation
 *
 * Implements RFC 1928 (SOCKS5) protocol for outbound proxy connections.
 * Supports:
 *   - CONNECT command (TCP stream forwarding)
 *   - Username/password authentication (RFC 1929)
 *   - IPv4, IPv6, and domain name destination addresses
 *
 * Typical usage: socks5_connect(&srv, hostname, port) returns a
 * socket fd connected through the proxy.
 */

#define KERNEL_INTERNAL
#include "socks5.h"
#include "socket.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "net.h"

/* SOCKS5 protocol constants */
#define SOCKS5_VER        0x05
#define SOCKS5_CMD_CONNECT   0x01
#define SOCKS5_CMD_BIND      0x02
#define SOCKS5_CMD_UDP       0x03
#define SOCKS5_ATYP_IPV4     0x01
#define SOCKS5_ATYP_DOMAIN   0x03
#define SOCKS5_ATYP_IPV6     0x04
#define SOCKS5_AUTH_NONE     0x00
#define SOCKS5_AUTH_PASSWD   0x02
#define SOCKS5_AUTH_NO_ACCEPT 0xFF

/* SOCKS5 reply codes */
#define SOCKS5_REP_SUCCESS   0x00
#define SOCKS5_REP_FAILURE   0x01

/* ── Server connection ─────────────────────────────────────────────── */

int socks5_connect(const struct socks5_server *srv,
                   const char *dest_host, uint16_t dest_port)
{
    if (!srv || !dest_host) return -EINVAL;

    /* Create TCP socket to proxy server */
    int fd = socket_connect(srv->host, srv->port, SOCK_STREAM);
    if (fd < 0) return fd;

    /* ── Step 1: Authentication negotiation ── */
    uint8_t greet_req[4];
    uint8_t greet_resp[2];

    greet_req[0] = SOCKS5_VER;
    greet_req[1] = 1;  /* number of auth methods */
    greet_req[2] = srv->password[0] ? SOCKS5_AUTH_PASSWD : SOCKS5_AUTH_NONE;

    if (socket_write(fd, greet_req, 3) != 3) {
        socket_close(fd);
        return -EIO;
    }

    if (socket_read(fd, greet_resp, 2) != 2) {
        socket_close(fd);
        return -EIO;
    }

    if (greet_resp[0] != SOCKS5_VER) {
        socket_close(fd);
        return -EPROTO;
    }

    /* Handle authentication if required */
    if (greet_resp[1] == SOCKS5_AUTH_PASSWD) {
        if (!srv->username[0] || !srv->password[0]) {
            socket_close(fd);
            return -EPERM;
        }
        /* RFC 1929: Username/password subnegotiation */
        uint8_t auth_req[512];
        int pos = 0;
        auth_req[pos++] = 0x01;  /* version */
        auth_req[pos++] = (uint8_t)strlen(srv->username);
        memcpy(&auth_req[pos], srv->username, strlen(srv->username));
        pos += (int)strlen(srv->username);
        auth_req[pos++] = (uint8_t)strlen(srv->password);
        memcpy(&auth_req[pos], srv->password, strlen(srv->password));
        pos += (int)strlen(srv->password);

        if (socket_write(fd, auth_req, pos) != pos) {
            socket_close(fd);
            return -EIO;
        }

        uint8_t auth_resp[2];
        if (socket_read(fd, auth_resp, 2) != 2 || auth_resp[1] != 0) {
            socket_close(fd);
            return -EPERM;
        }
    } else if (greet_resp[1] != SOCKS5_AUTH_NONE) {
        socket_close(fd);
        return -ENOSYS;
    }

    /* ── Step 2: CONNECT request ── */
    uint8_t conn_req[512];
    int pos = 0;
    conn_req[pos++] = SOCKS5_VER;
    conn_req[pos++] = SOCKS5_CMD_CONNECT;
    conn_req[pos++] = 0x00;  /* reserved */
    conn_req[pos++] = SOCKS5_ATYP_DOMAIN;
    conn_req[pos++] = (uint8_t)strlen(dest_host);
    memcpy(&conn_req[pos], dest_host, strlen(dest_host));
    pos += (int)strlen(dest_host);
    conn_req[pos++] = (uint8_t)(dest_port >> 8);
    conn_req[pos++] = (uint8_t)(dest_port & 0xFF);

    if (socket_write(fd, conn_req, pos) != pos) {
        socket_close(fd);
        return -EIO;
    }

    /* Read response (variable length, we read up to 256 bytes) */
    uint8_t conn_resp[256];
    int resp_len = socket_read(fd, conn_resp, 4);  /* read header first */
    if (resp_len < 4) {
        socket_close(fd);
        return -EIO;
    }

    if (conn_resp[0] != SOCKS5_VER || conn_resp[1] != SOCKS5_REP_SUCCESS) {
        socket_close(fd);
        return -ECONNREFUSED;
    }

    /* Read remaining response based on address type */
    int remaining = 0;
    switch (conn_resp[3]) {
    case SOCKS5_ATYP_IPV4:
        remaining = 4 + 2; break;  /* IPv4 + port */
    case SOCKS5_ATYP_IPV6:
        remaining = 16 + 2; break; /* IPv6 + port */
    case SOCKS5_ATYP_DOMAIN: {
        /* Need to read domain length first */
        if (resp_len < 5) {
            if (socket_read(fd, &conn_resp[4], 1) != 1) {
                socket_close(fd);
                return -EIO;
            }
            resp_len = 5;
        }
        remaining = conn_resp[4] + 2; /* domain + port */
        break;
    }
    default:
        socket_close(fd);
        return -EPROTO;
    }

    while (resp_len < 4 + remaining) {
        int n = socket_read(fd, &conn_resp[resp_len], 256 - resp_len);
        if (n <= 0) { socket_close(fd); return -EIO; }
        resp_len += n;
    }

    kprintf("[SOCKS5] Connected via %s:%u -> %s:%u\n",
            srv->host, srv->port, dest_host, dest_port);
    return fd;  /* return connected socket fd */
}

/* SOCKS5 BIND support — bind to a port on the proxy and accept connections */
int socks5_bind(const struct socks5_server *srv,
                const char *dest_host, uint16_t dest_port,
                uint16_t *bind_port)
{
    if (!srv || !dest_host || !bind_port) return -EINVAL;

    int fd = socket_connect(srv->host, srv->port, SOCK_STREAM);
    if (fd < 0) return fd;

    /* Step 1: Authentication (same as connect) */
    uint8_t greet_req[4];
    uint8_t greet_resp[2];
    greet_req[0] = SOCKS5_VER;
    greet_req[1] = 1;
    greet_req[2] = srv->password[0] ? SOCKS5_AUTH_PASSWD : SOCKS5_AUTH_NONE;
    if (socket_write(fd, greet_req, 3) != 3) { socket_close(fd); return -EIO; }
    if (socket_read(fd, greet_resp, 2) != 2) { socket_close(fd); return -EIO; }
    if (greet_resp[0] != SOCKS5_VER) { socket_close(fd); return -EPROTO; }
    if (greet_resp[1] == SOCKS5_AUTH_PASSWD) {
        if (!srv->username[0] || !srv->password[0]) { socket_close(fd); return -EPERM; }
        uint8_t auth_req[512];
        int pos = 0;
        auth_req[pos++] = 0x01;
        auth_req[pos++] = (uint8_t)strlen(srv->username);
        memcpy(&auth_req[pos], srv->username, strlen(srv->username));
        pos += (int)strlen(srv->username);
        auth_req[pos++] = (uint8_t)strlen(srv->password);
        memcpy(&auth_req[pos], srv->password, strlen(srv->password));
        pos += (int)strlen(srv->password);
        if (socket_write(fd, auth_req, pos) != pos) { socket_close(fd); return -EIO; }
        uint8_t auth_resp[2];
        if (socket_read(fd, auth_resp, 2) != 2 || auth_resp[1] != 0) { socket_close(fd); return -EPERM; }
    } else if (greet_resp[1] != SOCKS5_AUTH_NONE) { socket_close(fd); return -ENOSYS; }

    /* Step 2: BIND request */
    uint8_t bind_req[512];
    int pos = 0;
    bind_req[pos++] = SOCKS5_VER;
    bind_req[pos++] = SOCKS5_CMD_BIND;
    bind_req[pos++] = 0x00;
    bind_req[pos++] = SOCKS5_ATYP_DOMAIN;
    bind_req[pos++] = (uint8_t)strlen(dest_host);
    memcpy(&bind_req[pos], dest_host, strlen(dest_host));
    pos += (int)strlen(dest_host);
    bind_req[pos++] = (uint8_t)(dest_port >> 8);
    bind_req[pos++] = (uint8_t)(dest_port & 0xFF);

    if (socket_write(fd, bind_req, pos) != pos) { socket_close(fd); return -EIO; }

    /* Read first BIND response (bind address/port assigned by proxy) */
    uint8_t bind_resp[256];
    int resp_len = socket_read(fd, bind_resp, 4);
    if (resp_len < 4) { socket_close(fd); return -EIO; }
    if (bind_resp[0] != SOCKS5_VER || bind_resp[1] != SOCKS5_REP_SUCCESS) {
        socket_close(fd); return -ECONNREFUSED;
    }

    /* Parse the bound port */
    int remaining = 0;
    switch (bind_resp[3]) {
    case SOCKS5_ATYP_IPV4: remaining = 4 + 2; break;
    case SOCKS5_ATYP_IPV6: remaining = 16 + 2; break;
    case SOCKS5_ATYP_DOMAIN:
        if (resp_len < 5) {
            if (socket_read(fd, &bind_resp[4], 1) != 1) { socket_close(fd); return -EIO; }
            resp_len = 5;
        }
        remaining = bind_resp[4] + 2;
        break;
    default: socket_close(fd); return -EPROTO;
    }
    while (resp_len < 4 + remaining) {
        int n = socket_read(fd, &bind_resp[resp_len], 256 - resp_len);
        if (n <= 0) { socket_close(fd); return -EIO; }
        resp_len += n;
    }

    /* Extract bound port from the response */
    if (bind_resp[3] == SOCKS5_ATYP_IPV4) {
        *bind_port = (uint16_t)(bind_resp[8] << 8) | bind_resp[9];
    }

    /* Read second BIND response (incoming connection notification) */
    resp_len = socket_read(fd, bind_resp, 4);
    if (resp_len < 4) { socket_close(fd); return -EIO; }

    kprintf("[SOCKS5] BIND via %s:%u for %s:%u, bound port=%u\n",
            srv->host, srv->port, dest_host, dest_port, *bind_port);
    return fd;
}

/* SOCKS5 UDP ASSOCIATE support — create UDP association through proxy */
int socks5_udp_associate(const struct socks5_server *srv,
                          const char *dest_host, uint16_t dest_port,
                          uint16_t *assoc_port)
{
    if (!srv || !assoc_port) return -EINVAL;

    int fd = socket_connect(srv->host, srv->port, SOCK_STREAM);
    if (fd < 0) return fd;

    /* Step 1: Authentication */
    uint8_t greet_req[4];
    uint8_t greet_resp[2];
    greet_req[0] = SOCKS5_VER;
    greet_req[1] = 1;
    greet_req[2] = srv->password[0] ? SOCKS5_AUTH_PASSWD : SOCKS5_AUTH_NONE;
    if (socket_write(fd, greet_req, 3) != 3) { socket_close(fd); return -EIO; }
    if (socket_read(fd, greet_resp, 2) != 2) { socket_close(fd); return -EIO; }
    if (greet_resp[0] != SOCKS5_VER) { socket_close(fd); return -EPROTO; }
    if (greet_resp[1] == SOCKS5_AUTH_PASSWD) {
        if (!srv->username[0] || !srv->password[0]) { socket_close(fd); return -EPERM; }
        uint8_t auth_req[512];
        int pos = 0;
        auth_req[pos++] = 0x01;
        auth_req[pos++] = (uint8_t)strlen(srv->username);
        memcpy(&auth_req[pos], srv->username, strlen(srv->username));
        pos += (int)strlen(srv->username);
        auth_req[pos++] = (uint8_t)strlen(srv->password);
        memcpy(&auth_req[pos], srv->password, strlen(srv->password));
        pos += (int)strlen(srv->password);
        if (socket_write(fd, auth_req, pos) != pos) { socket_close(fd); return -EIO; }
        uint8_t auth_resp[2];
        if (socket_read(fd, auth_resp, 2) != 2 || auth_resp[1] != 0) { socket_close(fd); return -EPERM; }
    } else if (greet_resp[1] != SOCKS5_AUTH_NONE) { socket_close(fd); return -ENOSYS; }

    /* Step 2: UDP ASSOCIATE request */
    uint8_t udp_req[512];
    int pos = 0;
    udp_req[pos++] = SOCKS5_VER;
    udp_req[pos++] = SOCKS5_CMD_UDP;
    udp_req[pos++] = 0x00;
    /* Use domain name if provided, otherwise use zeros (all addresses) */
    if (dest_host && dest_host[0]) {
        udp_req[pos++] = SOCKS5_ATYP_DOMAIN;
        udp_req[pos++] = (uint8_t)strlen(dest_host);
        memcpy(&udp_req[pos], dest_host, strlen(dest_host));
        pos += (int)strlen(dest_host);
    } else {
        udp_req[pos++] = SOCKS5_ATYP_IPV4;
        udp_req[pos++] = 0; udp_req[pos++] = 0; udp_req[pos++] = 0; udp_req[pos++] = 0;
    }
    udp_req[pos++] = (uint8_t)(dest_port >> 8);
    udp_req[pos++] = (uint8_t)(dest_port & 0xFF);

    if (socket_write(fd, udp_req, pos) != pos) { socket_close(fd); return -EIO; }

    /* Read UDP ASSOCIATE response */
    uint8_t udp_resp[256];
    int resp_len = socket_read(fd, udp_resp, 4);
    if (resp_len < 4) { socket_close(fd); return -EIO; }
    if (udp_resp[0] != SOCKS5_VER || udp_resp[1] != SOCKS5_REP_SUCCESS) {
        socket_close(fd); return -ECONNREFUSED;
    }

    int remaining = 0;
    switch (udp_resp[3]) {
    case SOCKS5_ATYP_IPV4: remaining = 4 + 2; break;
    case SOCKS5_ATYP_IPV6: remaining = 16 + 2; break;
    case SOCKS5_ATYP_DOMAIN:
        if (resp_len < 5) {
            if (socket_read(fd, &udp_resp[4], 1) != 1) { socket_close(fd); return -EIO; }
            resp_len = 5;
        }
        remaining = udp_resp[4] + 2;
        break;
    default: socket_close(fd); return -EPROTO;
    }
    while (resp_len < 4 + remaining) {
        int n = socket_read(fd, &udp_resp[resp_len], 256 - resp_len);
        if (n <= 0) { socket_close(fd); return -EIO; }
        resp_len += n;
    }

    if (udp_resp[3] == SOCKS5_ATYP_IPV4) {
        *assoc_port = (uint16_t)(udp_resp[8] << 8) | udp_resp[9];
    }

    kprintf("[SOCKS5] UDP ASSOCIATE via %s:%u, assoc_port=%u\n",
            srv->host, srv->port, *assoc_port);
    return fd;
}

/* ── Initialization ─────────────────────────────────────────────────── */

void socks5_init(void)
{
    kprintf("[OK] SOCKS5 proxy client initialized\n");
}
#include "module.h"
module_init(socks5_init);

/* ── Stub: socks5_send ─────────────────────────────── */
int socks5_send(const void *data, size_t len)
{
    (void)data;
    (void)len;
    kprintf("[socks5] socks5_send: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: socks5_recv ─────────────────────────────── */
int socks5_recv(void *buf, size_t len)
{
    (void)buf;
    (void)len;
    kprintf("[socks5] socks5_recv: not yet implemented\n");
    return -ENOSYS;
}
