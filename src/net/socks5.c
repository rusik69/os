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

/* ── Initialization ─────────────────────────────────────────────────── */

void socks5_init(void)
{
    kprintf("[OK] SOCKS5 proxy client initialized\n");
}
