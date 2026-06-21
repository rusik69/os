// SPDX-License-Identifier: GPL-2.0-only
/*
 * vsock.c — VM Sockets for host-guest communication
 *
 * Provides a communication channel between host and guest VMs
 * using the VM Sockets protocol (AF_VSOCK).
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"

#define VSOCK_MAX_PORTS 128
#define VSOCK_BUFFER_SIZE 65536

struct vsock_port {
    int in_use;
    uint32_t port;
    int (*handler)(const uint8_t *data, size_t len);
};

static struct vsock_port vsock_ports[VSOCK_MAX_PORTS];

int vsock_register_port(uint32_t port, int (*handler)(const uint8_t *, size_t))
{
    if (port >= VSOCK_MAX_PORTS)
        return -EINVAL;
    if (vsock_ports[port].in_use)
        return -EADDRINUSE;

    vsock_ports[port].port = port;
    vsock_ports[port].handler = handler;
    vsock_ports[port].in_use = 1;
    return 0;
}

int vsock_unregister_port(uint32_t port)
{
    if (port >= VSOCK_MAX_PORTS || !vsock_ports[port].in_use)
        return -EINVAL;

    vsock_ports[port].in_use = 0;
    vsock_ports[port].handler = NULL;
    return 0;
}

int vsock_send(uint32_t dst_port, uint32_t src_port,
               const uint8_t *data, size_t len)
{
    (void)src_port;
    kprintf("[VSOCK] send: dst=%u len=%zu\n", dst_port, len);

    if (dst_port < VSOCK_MAX_PORTS && vsock_ports[dst_port].in_use) {
        if (vsock_ports[dst_port].handler)
            return vsock_ports[dst_port].handler(data, len);
    }
    return 0;
}

int vsock_recv(uint32_t port, uint8_t *buf, size_t max_len)
{
    (void)port;
    (void)buf;
    (void)max_len;
    kprintf("[VSOCK] recv on port %u\n", port);
    return 0;
}

void vsock_init(void)
{
    memset(vsock_ports, 0, sizeof(vsock_ports));
    kprintf("[OK] VSOCK — VM Sockets for host-guest communication\n");
}
#include "module.h"
module_init(vsock_init);

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: vsock_connect ───────────────────────────── */
int vsock_connect(uint32_t cid, uint32_t port)
{
    (void)cid;
    (void)port;
    kprintf("[VSOCK] vsock_connect: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: vsock_listen ────────────────────────────── */
int vsock_listen(uint32_t port, int backlog)
{
    (void)port;
    (void)backlog;
    kprintf("[VSOCK] vsock_listen: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: vsock_accept ────────────────────────────── */
int vsock_accept(uint32_t port)
{
    (void)port;
    kprintf("[VSOCK] vsock_accept: not yet implemented\n");
    return -ENOSYS;
}
