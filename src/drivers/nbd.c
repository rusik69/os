/*
 * src/drivers/nbd.c — Network Block Device (NBD) client.
 *
 * Connects to an NBD server on TCP port 10809, negotiates the export,
 * and exposes the remote storage as a local block device through the
 * blockdev framework.
 *
 * Uses net_tcp_connect() / net_tcp_send() / net_tcp_recv() from
 * src/net/net_tcp.c for TCP communication.
 */

#define KERNEL_INTERNAL
#include "nbd.h"
#include "types.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "blockdev.h"
#include "net.h"
#include "errno.h"

/* 64-bit byte-order helpers (NBD protocol is big-endian / network byte order,
 * x86-64 is little-endian — all multi-byte fields must be swapped) */
#define ntohll(v)  ((uint64_t)__builtin_bswap64((uint64_t)(v)))
#define htonll(v)  ((uint64_t)__builtin_bswap64((uint64_t)(v)))

/* Maximum number of NBD devices */
#define NBD_MAX_DEVICES 4

/* Per-device state */
static struct nbd_device g_nbd_devices[NBD_MAX_DEVICES];
static int g_nbd_initialized = 0;

/* ── Helpers: send/recv over TCP ────────────────────────────────────── */

/* Send raw bytes over TCP. Returns 0 on success, -1 on error. */
static int nbd_tcp_send(int conn_id, const void *data, uint32_t len)
{
    if (!net_tcp_is_connected(conn_id))
        return -1;

    int sent = net_tcp_send(conn_id, data, (uint16_t)len);
    if (sent != (int)len)
        return -1;
    return 0;
}

/* Receive exactly 'len' bytes over TCP (may need multiple recv calls). */
static int nbd_tcp_recv(int conn_id, void *buf, uint32_t len, int timeout_ticks)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = len;

    while (remaining > 0) {
        int n = net_tcp_recv(conn_id, p, (uint16_t)remaining, timeout_ticks);
        if (n <= 0)
            return -1;
        p += n;
        remaining -= (uint32_t)n;
    }
    return 0;
}

/* ── NBD old-style negotiation ──────────────────────────────────────── */

/*
 * Old-style negotiation:
 *   1. Server sends 128 bytes: magic (8) + export size (8) + flags (4) + reserved (108)
 *   2. Client sends the export name: uint32_t namelen in network byte order,
 *      followed by char name[namelen].  A zero-length name selects the
 *      server's default export.
 *   3. Server responds with 128 bytes of zeroes (acknowledgment).
 *   4. Now ready for read/write commands.
 */
static int nbd_negotiate(int conn_id, struct nbd_device *dev)
{
    uint8_t buf[128];

    /* Receive initial magic + export info */
    if (nbd_tcp_recv(conn_id, buf, 128, 100) < 0) {
        kprintf("[NBD] Negotiation failed: no initial data\n");
        return -1;
    }

    /* Compare magic as bytes (endian-safe — NBD_MAGIC on wire is "NBDMAGIC"
     * in network byte order; memcmp works on any host byte order) */
    if (memcmp(buf, "NBDMAGIC", 8) != 0) {
        kprintf("[NBD] Bad magic: expected \"NBDMAGIC\"\n");
        return -1;
    }

    /* All multi-byte NBD protocol fields are big-endian (network byte order);
     * x86-64 is little-endian, so swap to host byte order. */
    memcpy(&dev->export_size, buf + 8, sizeof(dev->export_size));
    dev->export_size = ntohll(dev->export_size);

    memcpy(&dev->flags, buf + 16, sizeof(dev->flags));
    dev->flags = ntohl(dev->flags);

    kprintf("[NBD] Export: %llu bytes, flags=0x%x\n",
            (unsigned long long)dev->export_size, dev->flags);

    /* Send export name: length 0 (empty name = default export).
     * NBD old-style protocol requires the client to send the export name
     * length in network byte order (big-endian) followed by the name.
     * A zero-length name selects the server's default export.
     * Note: htonl ensures the byte order is correct on little-endian
     * x86-64; for namelen=0 the result is identical regardless, but
     * using htonl prevents a latent bug if a non-zero name is used. */
    uint32_t namelen = htonl(0);
    if (nbd_tcp_send(conn_id, &namelen, 4) < 0) {
        kprintf("[NBD] Failed to send export name\n");
        return -1;
    }

    /* Receive server acknowledgment: 128 bytes of zeroes.
     * The server sends this after accepting the export name.
     * Not consuming it would desynchronise the protocol — the
     * acknowledgment would be read as the start of the first
     * command's reply. */
    uint8_t ack[128];
    if (nbd_tcp_recv(conn_id, ack, 128, 100) < 0) {
        kprintf("[NBD] Failed to receive export acknowledgment\n");
        return -1;
    }

    /* Verify acknowledgment is all zeros */
    for (int i = 0; i < 128; i++) {
        if (ack[i] != 0) {
            kprintf("[NBD] Export acknowledgment has non-zero byte at "
                    "offset %d — server rejected the export name\n", i);
            return -1;
        }
    }

    kprintf("[NBD] Negotiation complete\n");
    return 0;
}

/* ── Send an NBD command and receive the reply ──────────────────────── */

static int nbd_issue_command(int conn_id, uint32_t type, uint64_t offset,
                             uint32_t len, uint64_t handle,
                             void *data, int is_write)
{
    struct nbd_request req;
    struct nbd_reply   rep;

    /* Build request — all multi-byte fields in network byte order (big-endian) */
    req.magic  = htonl(NBD_REQUEST_MAGIC);
    req.type   = htonl(type);
    req.handle = htonll(handle);
    req.offset = htonll(offset);
    req.len    = htonl(len);

    /* Send request */
    if (nbd_tcp_send(conn_id, &req, sizeof(req)) < 0)
        return -EIO;

    /* For write: send data after request */
    if (is_write && data) {
        if (nbd_tcp_send(conn_id, data, len) < 0)
            return -EIO;
    }

    /* Receive reply */
    if (nbd_tcp_recv(conn_id, &rep, sizeof(rep), 100) < 0)
        return -EIO;

    /* Convert reply fields from network byte order to host byte order */
    rep.magic = ntohl(rep.magic);
    rep.error = ntohl(rep.error);
    rep.handle = ntohll(rep.handle);

    if (rep.magic != NBD_REPLY_MAGIC) {
        kprintf("[NBD] Bad reply magic: 0x%x\n", rep.magic);
        return -EIO;
    }

    if (rep.handle != handle) {
        kprintf("[NBD] Handle mismatch: %llx != %llx\n",
                (unsigned long long)rep.handle, (unsigned long long)handle);
        return -EIO;
    }

    /* For read: receive data after reply */
    if (!is_write && data && len > 0 && rep.error == 0) {
        if (nbd_tcp_recv(conn_id, data, len, 100) < 0)
            return -EIO;
    }

    return -(int)rep.error; /* 0 = success */
}

/* ── Find NBD device state by block device ID ───────────────────────── */

static struct nbd_device *nbd_find_by_dev_id(int dev_id)
{
    for (int i = 0; i < NBD_MAX_DEVICES; i++) {
        if (g_nbd_devices[i].connected && g_nbd_devices[i].dev_id == dev_id)
            return &g_nbd_devices[i];
    }
    return NULL;
}

/* ── Block device submit function ───────────────────────────────────── */

static int nbd_submit_fn(struct blk_request *req)
{
    if (!req) return -EINVAL;

    int dev_id = req->dev_id;
    struct nbd_device *dev = nbd_find_by_dev_id(dev_id);
    if (!dev || !dev->connected)
        return -ENODEV;

    /* Snapshot conn_id immediately after the connected check to prevent
     * a TOCTOU race with nbd_disconnect().  On SMP, another thread may
     * call nbd_disconnect() and memset the struct to zero between our
     * connected check and the conn_id read below.  By snapshoting the
     * value right here we guarantee we always use the conn_id that was
     * valid when the device was found connected, even if the struct is
     * zeroed concurrently.  Without this we could read conn_id = 0
     * (the post-memset value) and inadvertently send NBD commands to
     * TCP connection slot 0, which may belong to a different device. */
    int conn_id = dev->conn_id;

    uint64_t offset = req->lba * 512ULL;
    uint32_t len    = req->count * 512;
    int is_write    = (req->flags & BLK_REQ_WRITE);
    uint32_t type   = is_write ? NBD_CMD_WRITE : NBD_CMD_READ;
    uint64_t handle = (uint64_t)((uintptr_t)req);

    int ret = nbd_issue_command(conn_id, type, offset, len, handle,
                                req->buf, is_write);

    req->result = ret;
    return ret;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void nbd_init(void)
{
    if (g_nbd_initialized) return;
    memset(g_nbd_devices, 0, sizeof(g_nbd_devices));
    g_nbd_initialized = 1;
    kprintf("[NBD] NBD subsystem initialized (max %d devices)\n", NBD_MAX_DEVICES);
}

int nbd_connect(uint32_t server_ip)
{
    if (!g_nbd_initialized) nbd_init();

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < NBD_MAX_DEVICES; i++) {
        if (!g_nbd_devices[i].connected) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        kprintf("[NBD] No free device slots\n");
        return -1;
    }

    /* Connect to NBD server */
    int conn_id = net_tcp_connect(server_ip, NBD_PORT);
    if (conn_id < 0) {
        kprintf("[NBD] TCP connect failed to %d.%d.%d.%d:%d\n",
                NIPQUAD(server_ip), NBD_PORT);
        return -1;
    }

    kprintf("[NBD] Connected to %d.%d.%d.%d:%d (conn=%d)\n",
            NIPQUAD(server_ip), NBD_PORT, conn_id);

    /* Negotiate export */
    struct nbd_device *dev = &g_nbd_devices[slot];
    memset(dev, 0, sizeof(*dev));
    dev->conn_id = conn_id;

    if (nbd_negotiate(conn_id, dev) < 0) {
        net_tcp_close(conn_id);
        kprintf("[NBD] Negotiation failed\n");
        return -1;
    }

    /* Register as block device */
    int nbd_id = slot + 20; /* start NBD device IDs at 20 */
    dev->dev_id = nbd_id;

    char name[16];
    snprintf(name, sizeof(name), "nbd%d", slot);

    uint64_t sector_count = dev->export_size / 512;
    int ret = blockdev_register(nbd_id, name,
                                nbd_submit_fn, NULL,
                                sector_count, 0);
    if (ret != 0) {
        kprintf("[NBD] Failed to register block device %s\n", name);
        net_tcp_close(conn_id);
        memset(dev, 0, sizeof(*dev));
        return -1;
    }

    dev->connected = 1;
    kprintf("[NBD] Device %s (id=%d): %llu sectors\n",
            name, nbd_id, (unsigned long long)sector_count);

    return nbd_id;
}

void nbd_disconnect(int dev_id)
{
    struct nbd_device *dev = nbd_find_by_dev_id(dev_id);
    if (!dev) {
        kprintf("[NBD] Device %d not found\n", dev_id);
        return;
    }

    /* Mark disconnected immediately so nbd_find_by_dev_id() and
     * nbd_submit_fn() see the device as inactive during teardown.
     * The memset at the end zeros everything (including conn_id),
     * so we set connected = 0 up front to prevent any concurrent
     * lookup from treating the device as live while we clean up. */
    dev->connected = 0;

    blockdev_unregister(dev_id);
    net_tcp_close(dev->conn_id);
    kprintf("[NBD] Device nbd%d (id=%d) disconnected\n",
            (int)(dev - g_nbd_devices), dev_id);
    memset(dev, 0, sizeof(*dev));
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: nbd_xmit ────────────────────────────────── */
static int nbd_xmit(__maybe_unused struct nbd_device *dev, __maybe_unused struct nbd_request *req, __maybe_unused void *data)
{
    kprintf("[NBD] nbd_xmit: not yet implemented\n");
    return 0;
}
/* ── Stub: nbd_recv ────────────────────────────────── */
static int nbd_recv(__maybe_unused struct nbd_device *dev, __maybe_unused struct nbd_reply *rep, __maybe_unused void *data)
{
    kprintf("[NBD] nbd_recv: not yet implemented\n");
    return 0;
}
/* ── Stub: nbd_reconfigure ─────────────────────────── */
static int nbd_reconfigure(__maybe_unused struct nbd_device *dev, __maybe_unused uint64_t new_size)
{
    kprintf("[NBD] nbd_reconfigure: not yet implemented\n");
    return 0;
}
