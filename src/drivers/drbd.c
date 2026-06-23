/*
 * src/drivers/drbd.c — Distributed Replicated Block Device (DRBD).
 *
 * Implements a simple DRBD-style replication driver. Writes to the
 * local DRBD block device are mirrored to a peer over TCP. Protocol C
 * (sync): write completes only after both local and peer write succeed.
 *
 * Simple: single resource, no meta-disk, no dual-primary.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "drbd.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "blockdev.h"
#include "net.h"
#include "errno.h"
#include "spinlock.h"

/* ── Global state ──────────────────────────────────────────────────── */

static struct drbd_resource g_resources[DRBD_MAX_RESOURCES];
static int g_drbd_initialized = 0;
static spinlock_t g_drbd_lock;

/* ── Byte order helpers ─────────────────────────────────────────────── */

static inline uint16_t drbd_htons(uint16_t v) {
    return ((v >> 8) & 0xFF) | ((v << 8) & 0xFF00);
}

static inline uint32_t drbd_htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000U);
}

static inline uint64_t drbd_htonll(uint64_t v) {
    uint32_t hi = (uint32_t)(v >> 32);
    uint32_t lo = (uint32_t)(v & 0xFFFFFFFFULL);
    return ((uint64_t)drbd_htonl(lo) << 32) | (uint64_t)drbd_htonl(hi);
}

/* ── TCP send/recv helpers ──────────────────────────────────────────── */

static int drbd_tcp_send(struct drbd_resource *res,
                          const void *data, uint32_t len)
{
    if (!net_tcp_is_connected(res->conn_id))
        return -ENOTCONN;
    int sent = net_tcp_send(res->conn_id, data, len);
    return (sent == (int)len) ? 0 : -EIO;
}

static int drbd_tcp_recv(struct drbd_resource *res, void *buf,
                          uint32_t len, int timeout)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = len;
    while (remaining > 0) {
        int n = net_tcp_recv(res->conn_id, p, remaining, timeout);
        if (n <= 0) return -EIO;
        p += n;
        remaining -= (uint32_t)n;
    }
    return 0;
}

/* ── Send a packet to peer ──────────────────────────────────────────── */

static int drbd_send_packet(struct drbd_resource *res, uint16_t type,
                             uint64_t sector, uint32_t count,
                             const void *data, uint32_t data_len)
{
    struct drbd_packet_hdr hdr;
    uint32_t total_len = sizeof(hdr) + data_len;

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic  = drbd_htons(DRBD_MAGIC);
    hdr.type   = drbd_htons(type);
    hdr.len    = drbd_htonl(total_len);
    hdr.sector = drbd_htonll(sector);
    hdr.count  = drbd_htonl(count);
    hdr.seq    = drbd_htonl(res->last_seq++);

    if (drbd_tcp_send(res, &hdr, sizeof(hdr)) < 0)
        return -EIO;

    if (data_len > 0 && data) {
        if (drbd_tcp_send(res, data, data_len) < 0)
            return -EIO;
    }

    res->bytes_sent += total_len;
    return 0;
}

/* ── Receive a packet from peer ──────────────────────────────────────── */

static int drbd_recv_packet(struct drbd_resource *res,
                             struct drbd_packet_hdr *hdr,
                             void *data_buf, uint32_t *data_len)
{
    if (drbd_tcp_recv(res, hdr, sizeof(*hdr), 100) < 0)
        return -EIO;

    uint32_t total_len = drbd_htonl(hdr->len);
    uint32_t payload_len = total_len - sizeof(*hdr);

    if (payload_len > 0 && data_buf && payload_len <= *data_len) {
        if (drbd_tcp_recv(res, data_buf, payload_len, 100) < 0)
            return -EIO;
    }
    if (data_len)
        *data_len = payload_len;
    return 0;
}

/* ── Handle incoming packets from peer ───────────────────────────────── */

static void drbd_handle_packet(struct drbd_resource *res)
{
    struct drbd_packet_hdr hdr;
    uint8_t data_buf[4096];
    uint32_t data_len = sizeof(data_buf);

    if (drbd_recv_packet(res, &hdr, data_buf, &data_len) < 0) {
        kprintf("[drbd] Failed to receive packet\n");
        return;
    }

    uint16_t type = drbd_htons(hdr.type);

    switch (type) {
    case P_DATA:
        /* Peer sent data to replicate to us */
        kprintf("[drbd] Received P_DATA: sector=%llu count=%u\n",
                (unsigned long long)drbd_htonll(hdr.sector),
                drbd_htonl(hdr.count));

        /* Write to local backing store */
        if (res->local_dev_id >= 0) {
            int ret = blk_submit_sync(res->local_dev_id,
                                       drbd_htonll(hdr.sector),
                                       drbd_htonl(hdr.count),
                                       data_buf, BLK_REQ_WRITE);
            /* Send ACK */
            drbd_send_packet(res, P_ACK, hdr.sector,
                             drbd_htonl(hdr.count), NULL, 0);
            if (ret == 0) {
                res->writes_acked++;
            }
        }
        break;

    case P_ACK:
        /* Peer acknowledged our write */
        kprintf("[drbd] Received P_ACK: sector=%llu count=%u\n",
                (unsigned long long)drbd_htonll(hdr.sector),
                drbd_htonl(hdr.count));
        res->pending_writes--;
        break;

    case P_BARRIER:
        /* Barrier acknowledgment */
        kprintf("[drbd] Received P_BARRIER\n");
        drbd_send_packet(res, P_ACK, 0, 0, NULL, 0);
        break;

    default:
        kprintf("[drbd] Unknown packet type: 0x%04x\n", type);
        break;
    }
}

/* ── DRBD block device submit function ───────────────────────────────── */

static struct drbd_resource *drbd_find_by_dev_id(int dev_id)
{
    for (int i = 0; i < DRBD_MAX_RESOURCES; i++) {
        if (g_resources[i].active && g_resources[i].drbd_dev_id == dev_id)
            return &g_resources[i];
    }
    return NULL;
}

static int drbd_submit_fn(struct blk_request *req)
{
    if (!req) return -EINVAL;

    struct drbd_resource *res = drbd_find_by_dev_id(req->dev_id);
    if (!res || !res->active)
        return -ENODEV;

    /* Only mirror writes */
    if (!(req->flags & BLK_REQ_WRITE)) {
        /* Reads: just pass through to local backing store */
        return blk_submit_sync(res->local_dev_id, req->lba,
                                req->count, req->buf, BLK_REQ_READ);
    }

    uint32_t byte_len = req->count * DRBD_SECTOR_SIZE;

    /* Protocol C: write to local AND send to peer, wait for ACK */

    /* 1. Write locally */
    int local_ret = blk_submit_sync(res->local_dev_id, req->lba,
                                     req->count, req->buf, BLK_REQ_WRITE);
    if (local_ret < 0) {
        kprintf("[drbd] Local write failed: sector=%llu\n",
                (unsigned long long)req->lba);
        return local_ret;
    }

    /* 2. Replicate to peer (if connected) */
    if (res->conn_state == DRBD_STATE_CONNECTED) {
        res->pending_writes++;
        int peer_ret = drbd_send_packet(res, P_DATA, req->lba,
                                         req->count, req->buf, byte_len);
        if (peer_ret < 0) {
            kprintf("[drbd] Peer write failed\n");
            res->pending_writes--;
            return peer_ret;
        }

        /* 3. Wait for ACK from peer (simplified: poll for a bit) */
        int timeout = 100;
        while (res->pending_writes > 0 && timeout-- > 0) {
            /* Poll for incoming data */
            if (net_tcp_available(res->conn_id) > 0) {
                drbd_handle_packet(res);
            }
            for (volatile int i = 0; i < 10000; i++);
        }

        if (res->pending_writes > 0) {
            kprintf("[drbd] Timeout waiting for peer ACK\n");
            return -ETIMEDOUT;
        }

        res->writes_replicated++;
    }

    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void drbd_init(void)
{
    if (g_drbd_initialized) return;
    memset(g_resources, 0, sizeof(g_resources));
    spinlock_init(&g_drbd_lock);
    g_drbd_initialized = 1;
    kprintf("[drbd] DRBD subsystem initialized\n");
}

int drbd_create_resource(const char *name, int local_dev_id)
{
    if (!g_drbd_initialized) drbd_init();

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < DRBD_MAX_RESOURCES; i++) {
        if (!g_resources[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        kprintf("[drbd] No free resource slots\n");
        return -1;
    }

    struct drbd_resource *res = &g_resources[slot];
    memset(res, 0, sizeof(*res));

    if (name)
        strncpy(res->name, name, sizeof(res->name) - 1);
    res->local_dev_id = local_dev_id;
    res->conn_state = DRBD_STATE_STANDALONE;
    res->disk_state = DRBD_DISK_CONSISTENT;

    /* Register DRBD block device */
    int drbd_id = slot + 50;  /* Start DRBD IDs at 50 */
    res->drbd_dev_id = drbd_id;

    uint64_t sector_count = blockdev_get_sectors(local_dev_id);

    char drbd_name[16];
    snprintf(drbd_name, sizeof(drbd_name), "drbd%d", slot);

    int ret = blockdev_register(drbd_id, drbd_name,
                                 drbd_submit_fn, NULL,
                                 sector_count, 0);
    if (ret != 0) {
        kprintf("[drbd] Failed to register DRBD device\n");
        memset(res, 0, sizeof(*res));
        return -1;
    }

    res->active = 1;
    kprintf("[drbd] Resource '%s' created: drbd%d (id=%d), backing=%d, %llu sectors\n",
            res->name, slot, drbd_id, local_dev_id,
            (unsigned long long)sector_count);
    return slot;
}

int drbd_connect_peer(int res_id, uint32_t peer_ip, uint16_t port)
{
    if (res_id < 0 || res_id >= DRBD_MAX_RESOURCES || !g_resources[res_id].active)
        return -1;

    struct drbd_resource *res = &g_resources[res_id];

    if (port == 0) port = DRBD_PORT;

    /* Connect to peer */
    int conn_id = net_tcp_connect(peer_ip, port);
    if (conn_id < 0) {
        kprintf("[drbd] Failed to connect to peer %d.%d.%d.%d:%d\n",
                NIPQUAD(peer_ip), port);
        return -1;
    }

    res->conn_id = conn_id;
    res->peer_ip = peer_ip;
    res->peer_port = port;
    res->conn_state = DRBD_STATE_CONNECTED;

    kprintf("[drbd] Connected to peer %d.%d.%d.%d:%d on resource '%s'\n",
            NIPQUAD(peer_ip), port, res->name);
    return 0;
}

void drbd_disconnect(int res_id)
{
    if (res_id < 0 || res_id >= DRBD_MAX_RESOURCES || !g_resources[res_id].active)
        return;

    struct drbd_resource *res = &g_resources[res_id];

    if (res->conn_state >= DRBD_STATE_CONNECTED) {
        net_tcp_close(res->conn_id);
        res->conn_state = DRBD_STATE_STANDALONE;
    }

    kprintf("[drbd] Resource '%s' disconnected\n", res->name);
}

void drbd_poll(void)
{
    for (int i = 0; i < DRBD_MAX_RESOURCES; i++) {
        struct drbd_resource *res = &g_resources[i];
        if (!res->active || res->conn_state < DRBD_STATE_CONNECTED)
            continue;

        /* Check for incoming data */
        if (net_tcp_is_connected(res->conn_id) &&
            net_tcp_available(res->conn_id) > 0) {
            drbd_handle_packet(res);
        }

        /* Check if connection is still alive */
        if (!net_tcp_is_connected(res->conn_id)) {
            kprintf("[drbd] Connection lost for resource '%s'\n", res->name);
            res->conn_state = DRBD_STATE_STANDALONE;
        }
    }
}

int drbd_get_state(int res_id, int *conn_state, int *disk_state)
{
    if (res_id < 0 || res_id >= DRBD_MAX_RESOURCES || !g_resources[res_id].active)
        return -1;

    if (conn_state) *conn_state = g_resources[res_id].conn_state;
    if (disk_state) *disk_state = g_resources[res_id].disk_state;
    return 0;
}

/* ── Stub: drbd_connect ─────────────────────────────── */
int drbd_connect(__maybe_unused const char *peer)
{
    kprintf("[drbd] drbd_connect: not yet implemented\n");
    return 0;
}
/* ── Stub: drbd_replicate ─────────────────────────────── */
int drbd_replicate(__maybe_unused const void *data, __maybe_unused size_t len)
{
    kprintf("[drbd] drbd_replicate: not yet implemented\n");
    return 0;
}
