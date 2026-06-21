/*
 * src/drivers/nvmf.c — NVMe over Fabrics (NVMe-oF) target driver.
 *
 * Exports a local NVMe namespace over TCP (port 4420) using the
 * NVMe-oF transport protocol. Handles fabric connect, property
 * get/set, and admin/IO command submission.
 *
 * Simple implementation: single connection, no authentication,
 * no digest.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "nvmf.h"
#include "nvme.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "blockdev.h"
#include "net.h"
#include "errno.h"

/* ── Global state ──────────────────────────────────────────────────── */

static struct nvmf_target g_nvmf_target;
static int g_nvmf_init_done = 0;

/* ── Byte order helpers ─────────────────────────────────────────────── */

static inline uint16_t nvmf_htons(uint16_t v) {
    return ((v >> 8) & 0xFF) | ((v << 8) & 0xFF00);
}

static inline uint32_t nvmf_htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000U);
}

/* ── PDU helpers ────────────────────────────────────────────────────── */

static int nvmf_send_pdu(struct nvmf_target *tgt, const void *pdu, uint32_t len)
{
    if (!tgt->connected || !net_tcp_is_connected(tgt->conn_id))
        return -ENOTCONN;
    int sent = net_tcp_send(tgt->conn_id, pdu, len);
    return (sent == (int)len) ? 0 : -EIO;
}

static int nvmf_recv_exact(struct nvmf_target *tgt, void *buf, uint32_t len, int timeout)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = len;
    while (remaining > 0) {
        int n = net_tcp_recv(tgt->conn_id, p, remaining, timeout);
        if (n <= 0) return -EIO;
        p += n;
        remaining -= (uint32_t)n;
    }
    return 0;
}

/* ── Handle Fabrics CONNECT ──────────────────────────────────────────── */

static int nvmf_handle_connect(struct nvmf_target *tgt,
                                struct nvmf_pdu_hdr *req_hdr)
{
    struct nvmf_connect_cmd conn_req;
    struct nvmf_capsule_rsp rsp;
    uint8_t data_seg[256];
    uint32_t dlen;

    kprintf("[nvmf] Received Fabrics CONNECT\n");

    /* Read the connect command payload */
    if (nvmf_recv_exact(tgt, &conn_req, sizeof(conn_req), 100) < 0)
        return -EIO;

    /* Read optional data segment containing host/target NQN */
    dlen = req_hdr->len - sizeof(*req_hdr) - sizeof(conn_req);
    dlen = nvmf_htons((uint16_t)dlen);  /* len is big-endian */
    if (dlen > 0 && dlen <= sizeof(data_seg)) {
        if (nvmf_recv_exact(tgt, data_seg, dlen, 100) < 0)
            return -EIO;
    }

    kprintf("[nvmf] Connect: nsid=%u qid=%u\n",
            nvmf_htonl(conn_req.nsid), conn_req.cdw10 & 0xFFFF);

    /* Build connect response */
    struct nvmf_connect_rsp *conn_rsp = (struct nvmf_connect_rsp *)&rsp.cqe.cdw0;
    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.type = NVMF_PDU_IC_RSP;
    rsp.hdr.flags = 0x80;  /* SUCCESS */
    rsp.hdr.len = nvmf_htons((uint16_t)(sizeof(rsp) + 8));
    rsp.hdr.cid = req_hdr->cid;
    rsp.hdr.tag = req_hdr->tag;

    conn_rsp->cdw0 = 0;  /* SUCCESS */

    kprintf("[nvmf] Sending CONNECT response\n");
    return nvmf_send_pdu(tgt, &rsp, sizeof(rsp));
}

/* ── Handle Property Set ─────────────────────────────────────────────── */

static int nvmf_handle_prop_set(struct nvmf_target *tgt,
                                 struct nvmf_prop_set *prop)
{
    struct nvmf_capsule_rsp rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.type = NVMF_PDU_PROP_SET_RSP;
    rsp.hdr.flags = 0x80;
    rsp.hdr.len = nvmf_htons((uint16_t)sizeof(rsp));
    rsp.hdr.cid = prop->hdr.cid;
    rsp.hdr.tag = prop->hdr.tag;
    rsp.cqe.cdw0 = 0;  /* SUCCESS */

    kprintf("[nvmf] Property Set: offset=0x%x value=0x%x\n",
            nvmf_htonl(prop->offset), nvmf_htonl(prop->value));
    return nvmf_send_pdu(tgt, &rsp, sizeof(rsp));
}

/* ── Handle Property Get ─────────────────────────────────────────────── */

static int nvmf_handle_prop_get(struct nvmf_target *tgt,
                                 struct nvmf_prop_get *prop)
{
    struct nvmf_prop_get_rsp rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.type = NVMF_PDU_PROP_GET_RSP;
    rsp.hdr.flags = 0x80;
    rsp.hdr.len = nvmf_htons((uint16_t)sizeof(rsp));
    rsp.hdr.cid = prop->hdr.cid;
    rsp.hdr.tag = prop->hdr.tag;
    rsp.offset = prop->offset;
    rsp.value = 0;  /* Return 0 for simplicity */

    kprintf("[nvmf] Property Get: offset=0x%x\n", nvmf_htonl(prop->offset));
    return nvmf_send_pdu(tgt, &rsp, sizeof(rsp));
}

/* ── Submit an NVMe command (admin or I/O) ──────────────────────────── */

static int nvmf_submit_nvme_cmd(struct nvmf_target *tgt,
                                 struct nvmf_capsule_cmd *capsule)
{
    struct nvme_sq_entry sqe;
    struct nvme_cq_entry cqe;
    struct nvmf_capsule_rsp rsp;
    int ret;

    /* Build NVMe submission queue entry from capsule */
    memset(&sqe, 0, sizeof(sqe));
    sqe.cdw0 = nvmf_htonl(capsule->nvme_cdw0);
    sqe.nsid = nvmf_htonl(capsule->nsid);
    sqe.mptr = capsule->mptr;
    sqe.prp1 = capsule->prp1;
    sqe.prp2 = capsule->prp2;
    sqe.cdw10 = nvmf_htonl(capsule->cdw10_15[0]);
    sqe.cdw11 = nvmf_htonl(capsule->cdw10_15[1]);
    sqe.cdw12 = nvmf_htonl(capsule->cdw10_15[2]);
    sqe.cdw13 = nvmf_htonl(capsule->cdw10_15[3]);
    sqe.cdw14 = nvmf_htonl(capsule->cdw10_15[4]);
    sqe.cdw15 = nvmf_htonl(capsule->cdw10_15[5]);

    uint8_t opcode = (uint8_t)(sqe.cdw0 & 0xFF);

    /* Distinguish admin vs I/O commands */
    if (opcode == NVMF_FABRIC_COMMAND) {
        /* Already handled above */
        kprintf("[nvmf] Fabric command forwarded\n");
        return 0;
    }

    /* Submit via the NVMe driver's admin passthrough */
    /* For admin commands (opcode < 0x80 typically) */
    ret = nvme_submit_admin_cmd(&sqe, &cqe);

    /* Build response */
    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.type = NVMF_PDU_CAPSULE_RSP;
    rsp.hdr.flags = (ret == 0) ? 0x80 : 0x01;  /* SUCCESS or ERROR */
    rsp.hdr.len = nvmf_htons((uint16_t)sizeof(rsp));
    rsp.hdr.cid = capsule->hdr.cid;
    rsp.hdr.tag = capsule->hdr.tag;
    rsp.cqe = cqe;

    return nvmf_send_pdu(tgt, &rsp, sizeof(rsp));
}

/* ── Handle incoming capsule command ─────────────────────────────────── */

static int nvmf_handle_capsule_cmd(struct nvmf_target *tgt,
                                    struct nvmf_pdu_hdr *hdr)
{
    struct nvmf_capsule_cmd capsule;
    uint8_t extra_data[256];
    uint32_t total_len = nvmf_htons((uint16_t)hdr->len);

    memset(&capsule, 0, sizeof(capsule));

    /* Read the capsule command header */
    if (nvmf_recv_exact(tgt, (uint8_t *)&capsule + sizeof(*hdr),
                         sizeof(capsule) - sizeof(*hdr), 100) < 0)
        return -EIO;

    /* Read any extra data (for SGL etc.) */
    uint32_t extra = total_len - sizeof(capsule);
    if (extra > sizeof(extra_data)) extra = sizeof(extra_data);
    if (extra > 0) {
        if (nvmf_recv_exact(tgt, extra_data, extra, 100) < 0)
            return -EIO;
    }

    capsule.hdr = *hdr;  /* Copy the header */

    uint8_t opcode = (uint8_t)(capsule.nvme_cdw0 & 0xFF);

    kprintf("[nvmf] Capsule cmd: opcode=0x%02x nsid=%u\n",
            opcode, nvmf_htonl(capsule.nsid));

    /* If it's an admin command, submit locally */
    return nvmf_submit_nvme_cmd(tgt, &capsule);
}

/* ── Main PDU dispatch ──────────────────────────────────────────────── */

static void nvmf_dispatch_pdu(struct nvmf_target *tgt)
{
    struct nvmf_pdu_hdr hdr;

    if (nvmf_recv_exact(tgt, &hdr, sizeof(hdr), 100) < 0) {
        kprintf("[nvmf] Failed to receive PDU header\n");
        return;
    }

    switch (hdr.type) {
    case NVMF_PDU_IC_REQ:
        kprintf("[nvmf] Received IC_REQ (Fabric Connect)\n");
        nvmf_handle_connect(tgt, &hdr);
        break;
    case NVMF_PDU_CAPSULE_CMD:
        kprintf("[nvmf] Received Capsule Command\n");
        nvmf_handle_capsule_cmd(tgt, &hdr);
        break;
    case NVMF_PDU_PROP_SET:
        {
            struct nvmf_prop_set prop;
            memcpy(&prop, &hdr, sizeof(hdr));
            if (nvmf_recv_exact(tgt, (uint8_t *)&prop + sizeof(hdr),
                                 sizeof(prop) - sizeof(hdr), 100) < 0)
                return;
            nvmf_handle_prop_set(tgt, &prop);
        }
        break;
    case NVMF_PDU_PROP_GET:
        {
            struct nvmf_prop_get prop;
            memcpy(&prop, &hdr, sizeof(hdr));
            if (nvmf_recv_exact(tgt, (uint8_t *)&prop + sizeof(hdr),
                                 sizeof(prop) - sizeof(hdr), 100) < 0)
                return;
            nvmf_handle_prop_get(tgt, &prop);
        }
        break;
    default:
        kprintf("[nvmf] Unknown PDU type: 0x%02x\n", hdr.type);
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void nvmf_init(void)
{
    if (g_nvmf_init_done) return;
    memset(&g_nvmf_target, 0, sizeof(g_nvmf_target));
    g_nvmf_init_done = 1;
    kprintf("[nvmf] NVMe-oF target subsystem initialized\n");
}

int nvmf_start(uint16_t port, int nsid)
{
    if (!g_nvmf_init_done) nvmf_init();

    struct nvmf_target *tgt = &g_nvmf_target;
    if (tgt->active) {
        kprintf("[nvmf] Target already active\n");
        return -1;
    }

    tgt->nvme_nsid = nsid;

    /* We use net_tcp_listen to accept connections */
    tgt->port = port;
    tgt->active = 1;

    kprintf("[nvmf] Target starting on port %u, exporting NSID %d\n", port, nsid);
    return 0;
}

void nvmf_stop(void)
{
    struct nvmf_target *tgt = &g_nvmf_target;
    if (!tgt->active) return;

    if (tgt->connected) {
        net_tcp_close(tgt->conn_id);
        tgt->connected = 0;
    }
    tgt->active = 0;
    kprintf("[nvmf] Target stopped\n");
}

void nvmf_poll(void)
{
    struct nvmf_target *tgt = &g_nvmf_target;
    if (!tgt->active) return;

    /* Accept new connections using blocking accept */
    if (!tgt->connected) {
        /* Set up the TCP listener if not already listening */
        if (tgt->listen_fd <= 0) {
            const char *err = NULL;
            net_tcp_listen(tgt->port, NULL, NULL, NULL);
            int ret = tgt->port > 0 ? 0 : -1;
            if (ret < 0) {
                if (err) {
                    kprintf("[nvmf] listen on port %u failed: %s\n",
                            tgt->port, err);
                }
                return;
            }
            tgt->listen_fd = 1; /* mark as listening */
            kprintf("[nvmf] Listening on TCP port %u\n", tgt->port);
        }

        /* Try to accept a connection (short timeout so we don't block) */
        int conn = net_tcp_accept(tgt->port, 1); /* 1 tick timeout */
        if (conn > 0) {
            tgt->conn_id = conn;
            tgt->connected = 1;
            kprintf("[nvmf] Connection accepted (conn_id=%d)\n", conn);
        }
        return;
    }

    /* Process incoming PDUs */
    while (tgt->connected) {
        /* Check if data available */
        int avail = net_tcp_available(tgt->conn_id);
        if (avail <= 0) break;

        nvmf_dispatch_pdu(tgt);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: nvmf_connect ────────────────────────────── */
int nvmf_connect(const char *transport, const char *traddr, const char *nqn)
{
    (void)transport;
    (void)traddr;
    (void)nqn;
    kprintf("[nvmf] nvmf_connect: not yet implemented\n");
    return 0;
}
/* ── Stub: nvmf_disconnect ─────────────────────────── */
int nvmf_disconnect(void)
{
    kprintf("[nvmf] nvmf_disconnect: not yet implemented\n");
    return 0;
}
/* ── Stub: nvmf_send ───────────────────────────────── */
int nvmf_send(const void *data, uint32_t len)
{
    (void)data;
    (void)len;
    kprintf("[nvmf] nvmf_send: not yet implemented\n");
    return 0;
}
/* ── Stub: nvmf_recv ───────────────────────────────── */
int nvmf_recv(void *buf, uint32_t *len)
{
    (void)buf;
    (void)len;
    kprintf("[nvmf] nvmf_recv: not yet implemented\n");
    return 0;
}
