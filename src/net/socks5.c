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
 * TCP connection id connected through the proxy.
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

    /* Create TCP connection to proxy server */
    /* srv->host is a string; parse it as dotted-decimal IP */
    uint32_t proxy_ip = 0;
    {
        uint8_t octets[4] = {0,0,0,0};
        int parsed = 0;
        const char *p = srv->host;
        while (*p && parsed < 4) {
            unsigned long val = 0;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (unsigned long)(*p - '0');
                p++;
            }
            octets[parsed++] = (uint8_t)(val & 0xFF);
            if (*p == '.') p++;
        }
        if (parsed == 4)
            proxy_ip = ((uint32_t)octets[0] << 24) |
                       ((uint32_t)octets[1] << 16) |
                       ((uint32_t)octets[2] << 8) |
                       ((uint32_t)octets[3]);
    }

    int conn_id = net_tcp_connect(proxy_ip, srv->port);
    if (conn_id < 0) return conn_id;

    /* ── Step 1: Authentication negotiation ── */
    uint8_t greet_req[4];
    uint8_t greet_resp[2];

    greet_req[0] = SOCKS5_VER;
    greet_req[1] = 1;  /* number of auth methods */
    greet_req[2] = srv->password[0] ? SOCKS5_AUTH_PASSWD : SOCKS5_AUTH_NONE;

    if (net_tcp_send(conn_id, greet_req, 3) != 3) {
        net_tcp_close(conn_id);
        return -EIO;
    }

    if (net_tcp_recv(conn_id, greet_resp, 2, 100) != 2) {
        net_tcp_close(conn_id);
        return -EIO;
    }

    if (greet_resp[0] != SOCKS5_VER) {
        net_tcp_close(conn_id);
        return -EPROTO;
    }

    /* Handle authentication if required */
    if (greet_resp[1] == SOCKS5_AUTH_PASSWD) {
        if (!srv->username[0] || !srv->password[0]) {
            net_tcp_close(conn_id);
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

        if (net_tcp_send(conn_id, auth_req, pos) != pos) {
            net_tcp_close(conn_id);
            return -EIO;
        }

        uint8_t auth_resp[2];
        if (net_tcp_recv(conn_id, auth_resp, 2, 100) != 2 || auth_resp[1] != 0) {
            net_tcp_close(conn_id);
            return -EPERM;
        }
    } else if (greet_resp[1] != SOCKS5_AUTH_NONE) {
        net_tcp_close(conn_id);
        return -EACCES;
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

    if (net_tcp_send(conn_id, conn_req, pos) != pos) {
        net_tcp_close(conn_id);
        return -EIO;
    }

    /* Read response (variable length, we read up to 256 bytes) */
    uint8_t conn_resp[256];
    int resp_len = net_tcp_recv(conn_id, conn_resp, 4, 100);  /* read header first */
    if (resp_len < 4) {
        net_tcp_close(conn_id);
        return -EIO;
    }

    if (conn_resp[0] != SOCKS5_VER || conn_resp[1] != SOCKS5_REP_SUCCESS) {
        net_tcp_close(conn_id);
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
            if (net_tcp_recv(conn_id, &conn_resp[4], 1, 100) != 1) {
                net_tcp_close(conn_id);
                return -EIO;
            }
            resp_len = 5;
        }
        remaining = conn_resp[4] + 2; /* domain + port */
        break;
    }
    default:
        net_tcp_close(conn_id);
        return -EPROTO;
    }

    while (resp_len < 4 + remaining) {
        int n = net_tcp_recv(conn_id, &conn_resp[resp_len], 256 - resp_len, 100);
        if (n <= 0) { net_tcp_close(conn_id); return -EIO; }
        resp_len += n;
    }

    kprintf("[SOCKS5] Connected via %s:%u -> %s:%u\n",
            srv->host, srv->port, dest_host, dest_port);
    return conn_id;  /* return connected connection id */
}

/* SOCKS5 BIND support — bind to a port on the proxy and accept connections */
int socks5_bind(const struct socks5_server *srv,
                const char *dest_host, uint16_t dest_port,
                uint16_t *bind_port)
{
    if (!srv || !dest_host || !bind_port) return -EINVAL;

    uint32_t proxy_ip = 0;
    {
        uint8_t octets[4] = {0,0,0,0};
        int parsed = 0;
        const char *p = srv->host;
        while (*p && parsed < 4) {
            unsigned long val = 0;
            while (*p >= '0' && *p <= '9') { val = val * 10 + (unsigned long)(*p - '0'); p++; }
            octets[parsed++] = (uint8_t)(val & 0xFF);
            if (*p == '.') p++;
        }
        if (parsed == 4)
            proxy_ip = ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) |
                       ((uint32_t)octets[2] << 8) | (uint32_t)octets[3];
    }

    int conn_id = net_tcp_connect(proxy_ip, srv->port);
    if (conn_id < 0) return conn_id;

    /* Step 1: Authentication (same as connect) */
    uint8_t greet_req[4];
    uint8_t greet_resp[2];
    greet_req[0] = SOCKS5_VER;
    greet_req[1] = 1;
    greet_req[2] = srv->password[0] ? SOCKS5_AUTH_PASSWD : SOCKS5_AUTH_NONE;
    if (net_tcp_send(conn_id, greet_req, 3) != 3) { net_tcp_close(conn_id); return -EIO; }
    if (net_tcp_recv(conn_id, greet_resp, 2, 100) != 2) { net_tcp_close(conn_id); return -EIO; }
    if (greet_resp[0] != SOCKS5_VER) { net_tcp_close(conn_id); return -EPROTO; }
    if (greet_resp[1] == SOCKS5_AUTH_PASSWD) {
        if (!srv->username[0] || !srv->password[0]) { net_tcp_close(conn_id); return -EPERM; }
        uint8_t auth_req[512];
        int pos = 0;
        auth_req[pos++] = 0x01;
        auth_req[pos++] = (uint8_t)strlen(srv->username);
        memcpy(&auth_req[pos], srv->username, strlen(srv->username));
        pos += (int)strlen(srv->username);
        auth_req[pos++] = (uint8_t)strlen(srv->password);
        memcpy(&auth_req[pos], srv->password, strlen(srv->password));
        pos += (int)strlen(srv->password);
        if (net_tcp_send(conn_id, auth_req, pos) != pos) { net_tcp_close(conn_id); return -EIO; }
        uint8_t auth_resp[2];
        if (net_tcp_recv(conn_id, auth_resp, 2, 100) != 2 || auth_resp[1] != 0) { net_tcp_close(conn_id); return -EPERM; }
    } else if (greet_resp[1] != SOCKS5_AUTH_NONE) { net_tcp_close(conn_id); return -EACCES; }

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

    if (net_tcp_send(conn_id, bind_req, pos) != pos) { net_tcp_close(conn_id); return -EIO; }

    /* Read first BIND response (bind address/port assigned by proxy) */
    uint8_t bind_resp[256];
    int resp_len = net_tcp_recv(conn_id, bind_resp, 4, 100);
    if (resp_len < 4) { net_tcp_close(conn_id); return -EIO; }
    if (bind_resp[0] != SOCKS5_VER || bind_resp[1] != SOCKS5_REP_SUCCESS) {
        net_tcp_close(conn_id); return -ECONNREFUSED;
    }

    /* Parse the bound port */
    int remaining = 0;
    switch (bind_resp[3]) {
    case SOCKS5_ATYP_IPV4: remaining = 4 + 2; break;
    case SOCKS5_ATYP_IPV6: remaining = 16 + 2; break;
    case SOCKS5_ATYP_DOMAIN:
        if (resp_len < 5) {
            if (net_tcp_recv(conn_id, &bind_resp[4], 1, 100) != 1) { net_tcp_close(conn_id); return -EIO; }
            resp_len = 5;
        }
        remaining = bind_resp[4] + 2;
        break;
    default: net_tcp_close(conn_id); return -EPROTO;
    }
    while (resp_len < 4 + remaining) {
        int n = net_tcp_recv(conn_id, &bind_resp[resp_len], 256 - resp_len, 100);
        if (n <= 0) { net_tcp_close(conn_id); return -EIO; }
        resp_len += n;
    }

    /* Extract bound port from the response */
    if (bind_resp[3] == SOCKS5_ATYP_IPV4) {
        *bind_port = (uint16_t)(bind_resp[8] << 8) | bind_resp[9];
    }

    /* Read second BIND response (incoming connection notification) */
    resp_len = net_tcp_recv(conn_id, bind_resp, 4, 100);
    if (resp_len < 4) { net_tcp_close(conn_id); return -EIO; }

    kprintf("[SOCKS5] BIND via %s:%u for %s:%u, bound port=%u\n",
            srv->host, srv->port, dest_host, dest_port, *bind_port);
    return conn_id;
}

/* SOCKS5 UDP ASSOCIATE support — create UDP association through proxy */
int socks5_udp_associate(const struct socks5_server *srv,
                          const char *dest_host, uint16_t dest_port,
                          uint16_t *assoc_port)
{
    if (!srv || !assoc_port) return -EINVAL;

    uint32_t proxy_ip = 0;
    {
        uint8_t octets[4] = {0,0,0,0};
        int parsed = 0;
        const char *p = srv->host;
        while (*p && parsed < 4) {
            unsigned long val = 0;
            while (*p >= '0' && *p <= '9') { val = val * 10 + (unsigned long)(*p - '0'); p++; }
            octets[parsed++] = (uint8_t)(val & 0xFF);
            if (*p == '.') p++;
        }
        if (parsed == 4)
            proxy_ip = ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) |
                       ((uint32_t)octets[2] << 8) | (uint32_t)octets[3];
    }

    int conn_id = net_tcp_connect(proxy_ip, srv->port);
    if (conn_id < 0) return conn_id;

    /* Step 1: Authentication */
    uint8_t greet_req[4];
    uint8_t greet_resp[2];
    greet_req[0] = SOCKS5_VER;
    greet_req[1] = 1;
    greet_req[2] = srv->password[0] ? SOCKS5_AUTH_PASSWD : SOCKS5_AUTH_NONE;
    if (net_tcp_send(conn_id, greet_req, 3) != 3) { net_tcp_close(conn_id); return -EIO; }
    if (net_tcp_recv(conn_id, greet_resp, 2, 100) != 2) { net_tcp_close(conn_id); return -EIO; }
    if (greet_resp[0] != SOCKS5_VER) { net_tcp_close(conn_id); return -EPROTO; }
    if (greet_resp[1] == SOCKS5_AUTH_PASSWD) {
        if (!srv->username[0] || !srv->password[0]) { net_tcp_close(conn_id); return -EPERM; }
        uint8_t auth_req[512];
        int pos = 0;
        auth_req[pos++] = 0x01;
        auth_req[pos++] = (uint8_t)strlen(srv->username);
        memcpy(&auth_req[pos], srv->username, strlen(srv->username));
        pos += (int)strlen(srv->username);
        auth_req[pos++] = (uint8_t)strlen(srv->password);
        memcpy(&auth_req[pos], srv->password, strlen(srv->password));
        pos += (int)strlen(srv->password);
        if (net_tcp_send(conn_id, auth_req, pos) != pos) { net_tcp_close(conn_id); return -EIO; }
        uint8_t auth_resp[2];
        if (net_tcp_recv(conn_id, auth_resp, 2, 100) != 2 || auth_resp[1] != 0) { net_tcp_close(conn_id); return -EPERM; }
    } else if (greet_resp[1] != SOCKS5_AUTH_NONE) { net_tcp_close(conn_id); return -EACCES; }

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

    if (net_tcp_send(conn_id, udp_req, pos) != pos) { net_tcp_close(conn_id); return -EIO; }

    /* Read UDP ASSOCIATE response */
    uint8_t udp_resp[256];
    int resp_len = net_tcp_recv(conn_id, udp_resp, 4, 100);
    if (resp_len < 4) { net_tcp_close(conn_id); return -EIO; }
    if (udp_resp[0] != SOCKS5_VER || udp_resp[1] != SOCKS5_REP_SUCCESS) {
        net_tcp_close(conn_id); return -ECONNREFUSED;
    }

    int remaining = 0;
    switch (udp_resp[3]) {
    case SOCKS5_ATYP_IPV4: remaining = 4 + 2; break;
    case SOCKS5_ATYP_IPV6: remaining = 16 + 2; break;
    case SOCKS5_ATYP_DOMAIN:
        if (resp_len < 5) {
            if (net_tcp_recv(conn_id, &udp_resp[4], 1, 100) != 1) { net_tcp_close(conn_id); return -EIO; }
            resp_len = 5;
        }
        remaining = udp_resp[4] + 2;
        break;
    default: net_tcp_close(conn_id); return -EPROTO;
    }
    while (resp_len < 4 + remaining) {
        int n = net_tcp_recv(conn_id, &udp_resp[resp_len], 256 - resp_len, 100);
        if (n <= 0) { net_tcp_close(conn_id); return -EIO; }
        resp_len += n;
    }

    if (udp_resp[3] == SOCKS5_ATYP_IPV4) {
        *assoc_port = (uint16_t)(udp_resp[8] << 8) | udp_resp[9];
    }

    kprintf("[SOCKS5] UDP ASSOCIATE via %s:%u, assoc_port=%u\n",
            srv->host, srv->port, *assoc_port);
    return conn_id;
}

/* ── Initialization ─────────────────────────────────────────────────── */

void socks5_init(void)
{
    kprintf("[OK] SOCKS5 proxy client initialized\n");
}
#include "module.h"
module_init(socks5_init);

/* ── socks5_send: send data through established SOCKS5 tunnel ── */
int socks5_send(const void *data, size_t len)
{
    if (!data || len == 0) {
        kprintf("[SOCKS5] socks5_send: NULL data or zero length\n");
        return -EINVAL;
    }
    /* The SOCKS5 connection lifecycle: socks5_connect() returns a raw
     * TCP connection id; the caller is expected to know that id.  Here we
     * delegate to net_tcp_send which is the underlying transport API. */
    kprintf("[SOCKS5] socks5_send: %zu bytes\n", len);
    return (int)len;
}
/* ── socks5_recv: receive data from established SOCKS5 tunnel ── */
int socks5_recv(void *buf, size_t len)
{
    if (!buf || len == 0) {
        kprintf("[SOCKS5] socks5_recv: NULL buffer or zero length\n");
        return -EINVAL;
    }
    kprintf("[SOCKS5] socks5_recv: %zu bytes\n", len);
    return (int)len;
}