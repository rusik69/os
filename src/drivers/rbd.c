/*
 * src/drivers/rbd.c — Ceph RADOS Block Device (RBD) client.
 *
 * Connects to a Ceph cluster via a simplified OSD map (single OSD),
 * reads/writes RADOS objects using OSD_OP_READ/WRITE, and maps
 * RBD block device to RADOS objects (each 4M).
 *
 * Simple: single OSD, no auth (cephx), no striping.
 */

#define KERNEL_INTERNAL
#include "rbd.h"
#include "types.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "blockdev.h"
#include "net.h"
#include "errno.h"

/* ── Global state ──────────────────────────────────────────────────── */

static struct rbd_device g_rbd_devices[RBD_MAX_DEVICES];
static int g_rbd_initialized = 0;

/* ── Byte order helpers ─────────────────────────────────────────────── */

static inline uint16_t rbd_htons(uint16_t v) {
    return ((v >> 8) & 0xFF) | ((v << 8) & 0xFF00);
}
static inline uint32_t rbd_htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000U);
}
static inline uint64_t rbd_htonll(uint64_t v) {
    uint32_t hi = (uint32_t)(v >> 32);
    uint32_t lo = (uint32_t)(v & 0xFFFFFFFFULL);
    return ((uint64_t)rbd_htonl(lo) << 32) | (uint64_t)rbd_htonl(hi);
}

/* ── TCP send/recv helpers ──────────────────────────────────────────── */

static int rbd_tcp_send(int conn_id, const void *data, uint32_t len)
{
    if (!net_tcp_is_connected(conn_id)) return -ENOTCONN;
    int sent = net_tcp_send(conn_id, data, (uint16_t)len);
    return (sent == (int)len) ? 0 : -EIO;
}

static int rbd_tcp_recv(int conn_id, void *buf, uint32_t len, int timeout)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = len;
    while (remaining > 0) {
        int n = net_tcp_recv(conn_id, p, (uint16_t)remaining, timeout);
        if (n <= 0) return -EIO;
        p += n;
        remaining -= (uint32_t)n;
    }
    return 0;
}

/* ── Ceph message framing ────────────────────────────────────────────── */

static int ceph_send_message(int conn_id, uint16_t type, uint64_t tid,
                              const void *front, uint32_t front_len,
                              const void *data, uint32_t data_len)
{
    struct ceph_msg_header hdr;
    struct ceph_msg_footer ftr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.seq = 0;
    hdr.tid = rbd_htonll(tid);
    hdr.type = rbd_htons(type);
    hdr.front_len = rbd_htonl(front_len);
    hdr.data_len = rbd_htonl(data_len);
    hdr.tag = CEPH_MSGR_TAG_MSG;

    if (rbd_tcp_send(conn_id, &hdr, sizeof(hdr)) < 0) return -EIO;
    if (front_len > 0 && front) {
        if (rbd_tcp_send(conn_id, front, front_len) < 0) return -EIO;
    }
    if (data_len > 0 && data) {
        if (rbd_tcp_send(conn_id, data, data_len) < 0) return -EIO;
    }

    memset(&ftr, 0, sizeof(ftr));
    if (rbd_tcp_send(conn_id, &ftr, sizeof(ftr)) < 0) return -EIO;

    return 0;
}

static int ceph_recv_message(int conn_id,
                              uint16_t *type_out, uint64_t *tid_out,
                              void *front_buf, uint32_t *front_len_out,
                              void *data_buf, uint32_t *data_len_out)
{
    struct ceph_msg_header hdr;
    struct ceph_msg_footer ftr;

    if (rbd_tcp_recv(conn_id, &hdr, sizeof(hdr), 100) < 0)
        return -EIO;

    uint32_t front_len = rbd_htonl(hdr.front_len);
    uint32_t data_len = rbd_htonl(hdr.data_len);

    if (type_out) *type_out = rbd_htons(hdr.type);
    if (tid_out) *tid_out = rbd_htonll(hdr.tid);

    if (front_len > 0 && front_buf && front_len <= *front_len_out) {
        if (rbd_tcp_recv(conn_id, front_buf, front_len, 100) < 0)
            return -EIO;
    }
    if (front_len_out) *front_len_out = front_len;

    if (data_len > 0 && data_buf && data_len <= *data_len_out) {
        if (rbd_tcp_recv(conn_id, data_buf, data_len, 100) < 0)
            return -EIO;
    }
    if (data_len_out) *data_len_out = data_len;

    /* Read footer */
    if (rbd_tcp_recv(conn_id, &ftr, sizeof(ftr), 100) < 0)
        return -EIO;

    return 0;
}

/* ── Ceph protocol negotiation (simplified) ─────────────────────────── */

static int ceph_connect(int conn_id)
{
    uint8_t banner[] = "ceph v0.1\n";
    uint8_t banner_resp[64];
    uint16_t msg_type;
    uint64_t msg_tid;
    uint32_t front_len;
    uint32_t data_len;

    /* Send banner */
    if (rbd_tcp_send(conn_id, banner, sizeof(banner) - 1) < 0)
        return -EIO;

    /* Receive banner response */
    memset(banner_resp, 0, sizeof(banner_resp));
    if (rbd_tcp_recv(conn_id, banner_resp, 64, 100) < 0)
        return -EIO;

    kprintf("[ceph] Connected, banner: %s\n", banner_resp);

    /* Send our entity info */
    uint8_t entity_info[32];
    memset(entity_info, 0, sizeof(entity_info));
    entity_info[0] = CEPH_ENTITY_TYPE_CLIENT;
    entity_info[1] = 0;  /* num */

    if (ceph_send_message(conn_id, 0x01, 0, entity_info, 32, NULL, 0) < 0)
        return -EIO;

    /* Wait for response */
    front_len = 256;
    data_len = 256;
    if (ceph_recv_message(conn_id, &msg_type, &msg_tid,
                           entity_info, &front_len,
                           NULL, &data_len) < 0)
        return -EIO;

    kprintf("[ceph] Protocol negotiation done, msg_type=%u\n", msg_type);
    return 0;
}

/* ── Connect to monitor and get OSD map ──────────────────────────────── */

static int rbd_connect_monitor(struct rbd_device *dev)
{
    dev->mon_conn_id = net_tcp_connect(dev->mon_ip, dev->mon_port);
    if (dev->mon_conn_id < 0) {
        kprintf("[rbd] Monitor connect failed\n");
        return -1;
    }

    if (ceph_connect(dev->mon_conn_id) < 0) {
        kprintf("[rbd] Monitor protocol negotiation failed\n");
        net_tcp_close(dev->mon_conn_id);
        return -1;
    }

    /* Simplified: just get the OSD address from monitor */
    kprintf("[rbd] Connected to monitor %d.%d.%d.%d:%d\n",
            NIPQUAD(dev->mon_ip), dev->mon_port);
    return 0;
}

/* ── Connect to OSD ──────────────────────────────────────────────────── */

static int rbd_connect_osd(struct rbd_device *dev)
{
    dev->osd_conn_id = net_tcp_connect(dev->osd_ip, dev->osd_port);
    if (dev->osd_conn_id < 0) {
        kprintf("[rbd] OSD connect failed\n");
        return -1;
    }

    if (ceph_connect(dev->osd_conn_id) < 0) {
        kprintf("[rbd] OSD protocol negotiation failed\n");
        net_tcp_close(dev->osd_conn_id);
        return -1;
    }

    kprintf("[rbd] Connected to OSD %d.%d.%d.%d:%d\n",
            NIPQUAD(dev->osd_ip), dev->osd_port);
    return 0;
}

/* ── RADOS object name for RBD ───────────────────────────────────────── */

static void rbd_object_name(struct rbd_device *dev,
                             uint64_t obj_no, char *buf, int buf_len)
{
    /* RBD: rbd_data.<pool_id>.<image_id>.<obj_no> */
    snprintf(buf, buf_len, "rbd_data.0.%llu",
             (unsigned long long)obj_no);
}

/* ── RADOS read/write operations ─────────────────────────────────────── */

static int rados_op(struct rbd_device *dev, int is_write,
                     uint64_t object_no, uint64_t offset,
                     void *data, uint32_t len)
{
    struct ceph_osd_request req;
    struct ceph_osd_response resp;
    uint8_t front_buf[128];
    uint32_t front_len, data_len;
    uint16_t msg_type;
    uint64_t tid;
    int ret;

    /* Build OSD request */
    memset(&req, 0, sizeof(req));
    req.op = rbd_htonl(is_write ? CEPH_OSD_OP_WRITE : CEPH_OSD_OP_READ);
    req.offset = rbd_htonll(offset);
    req.length = rbd_htonll((uint64_t)len);
    req.object_id = rbd_htonll(object_no);

    /* Send OSD request */
    static uint64_t global_tid = 1;
    tid = global_tid++;

    /* Front segment contains the request */
    memcpy(front_buf, &req, sizeof(req));

    ret = ceph_send_message(dev->osd_conn_id, 0x02, tid,
                            front_buf, sizeof(req),
                            is_write ? data : NULL,
                            is_write ? len : 0);
    if (ret < 0) return ret;

    /* Receive response */
    front_len = sizeof(front_buf);
    data_len = len;
    ret = ceph_recv_message(dev->osd_conn_id, &msg_type, &tid,
                            front_buf, &front_len,
                            !is_write ? data : NULL,
                            !is_write ? &data_len : NULL);
    if (ret < 0) return ret;

    /* Parse response */
    memcpy(&resp, front_buf, sizeof(resp));
    int32_t op_ret = rbd_htonl((uint32_t)resp.ret);
    if (op_ret < 0) {
        kprintf("[rados] OSD op failed: ret=%d\n", op_ret);
        return -EIO;
    }

    return 0;
}

/* ── Map block offset to RADOS object ────────────────────────────────── */

static void rbd_offset_to_object(struct rbd_device *dev,
                                  uint64_t byte_offset,
                                  uint64_t *obj_no,
                                  uint64_t *obj_offset,
                                  uint32_t *obj_len)
{
    *obj_no = byte_offset / RBD_OBJECT_SIZE;
    *obj_offset = byte_offset % RBD_OBJECT_SIZE;
    *obj_len = RBD_OBJECT_SIZE;
}

/* ── Block device submit function ─────────────────────────────────────── */

static struct rbd_device *rbd_find_by_dev_id(int dev_id)
{
    for (int i = 0; i < RBD_MAX_DEVICES; i++) {
        if (g_rbd_devices[i].connected && g_rbd_devices[i].dev_id == dev_id)
            return &g_rbd_devices[i];
    }
    return NULL;
}

static int rbd_submit_fn(struct blk_request *req)
{
    if (!req) return -EINVAL;

    struct rbd_device *dev = rbd_find_by_dev_id(req->dev_id);
    if (!dev || !dev->connected)
        return -ENODEV;

    int is_write = (req->flags & BLK_REQ_WRITE) ? 1 : 0;
    uint64_t byte_offset = req->lba * dev->sector_size;
    uint32_t byte_len = req->count * dev->sector_size;
    uint8_t *buf = (uint8_t *)req->buf;
    uint32_t remaining = byte_len;
    uint64_t cur_offset = byte_offset;
    int ret;

    /* Split across RADOS objects (each 4M) */
    while (remaining > 0) {
        uint64_t obj_no, obj_off;
        uint32_t obj_len;

        rbd_offset_to_object(dev, cur_offset, &obj_no, &obj_off, &obj_len);

        uint32_t chunk = obj_len - (uint32_t)obj_off;
        if (chunk > remaining) chunk = remaining;

        ret = rados_op(dev, is_write, obj_no, obj_off, buf, chunk);
        if (ret < 0) {
            req->result = ret;
            return ret;
        }

        buf += chunk;
        cur_offset += chunk;
        remaining -= chunk;
    }

    req->result = 0;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void rbd_init(void)
{
    if (g_rbd_initialized) return;
    memset(g_rbd_devices, 0, sizeof(g_rbd_devices));
    g_rbd_initialized = 1;
    kprintf("[rbd] RBD client subsystem initialized\n");
}

int rbd_connect(uint32_t mon_ip, uint32_t osd_ip)
{
    if (!g_rbd_initialized) rbd_init();

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < RBD_MAX_DEVICES; i++) {
        if (!g_rbd_devices[i].connected) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        kprintf("[rbd] No free device slots\n");
        return -1;
    }

    struct rbd_device *dev = &g_rbd_devices[slot];
    memset(dev, 0, sizeof(*dev));

    dev->mon_ip = mon_ip;
    dev->mon_port = CEPH_MON_PORT;
    dev->osd_ip = osd_ip;
    dev->osd_port = CEPH_OSD_PORT;

    /* Connect to monitor */
    if (rbd_connect_monitor(dev) < 0) {
        kprintf("[rbd] Monitor connection failed\n");
        return -1;
    }

    /* Connect to OSD */
    if (rbd_connect_osd(dev) < 0) {
        net_tcp_close(dev->mon_conn_id);
        kprintf("[rbd] OSD connection failed\n");
        return -1;
    }

    /* Simplified: assume 1GB image */
    dev->image_size = 1024ULL * 1024 * 1024;
    dev->sector_size = 512;
    dev->sector_count = dev->image_size / dev->sector_size;
    dev->num_objects = (dev->image_size + RBD_OBJECT_SIZE - 1) / RBD_OBJECT_SIZE;

    kprintf("[rbd] Image: %llu bytes, %llu sectors, %llu objects\n",
            (unsigned long long)dev->image_size,
            (unsigned long long)dev->sector_count,
            (unsigned long long)dev->num_objects);

    /* Register as block device */
    int rbd_id = slot + 60;  /* Start RBD IDs at 60 */
    dev->dev_id = rbd_id;

    char name[16];
    snprintf(name, sizeof(name), "rbd%d", slot);

    int ret = blockdev_register(rbd_id, name,
                                 rbd_submit_fn, NULL,
                                 dev->sector_count, 0);
    if (ret != 0) {
        kprintf("[rbd] Failed to register block device\n");
        net_tcp_close(dev->mon_conn_id);
        net_tcp_close(dev->osd_conn_id);
        memset(dev, 0, sizeof(*dev));
        return -1;
    }

    dev->connected = 1;
    kprintf("[rbd] Device %s (id=%d): %llu sectors\n",
            name, rbd_id, (unsigned long long)dev->sector_count);
    return rbd_id;
}

void rbd_disconnect(int dev_id)
{
    struct rbd_device *dev = rbd_find_by_dev_id(dev_id);
    if (!dev) return;

    blockdev_unregister(dev_id);
    if (dev->mon_conn_id >= 0) net_tcp_close(dev->mon_conn_id);
    if (dev->osd_conn_id >= 0) net_tcp_close(dev->osd_conn_id);
    kprintf("[rbd] Device rbd%d (id=%d) disconnected\n",
            (int)(dev - g_rbd_devices), dev_id);
    memset(dev, 0, sizeof(*dev));
}

/* ── Stub: rbd_read ─────────────────────────────── */
static int rbd_read(void *buf, size_t count, uint64_t offset)
{
    (void)buf;
    (void)count;
    (void)offset;
    kprintf("[rbd] rbd_read: not yet implemented\n");
    return 0;
}
/* ── Stub: rbd_write ─────────────────────────────── */
static int rbd_write(const void *buf, size_t count, uint64_t offset)
{
    (void)buf;
    (void)count;
    (void)offset;
    kprintf("[rbd] rbd_write: not yet implemented\n");
    return 0;
}
