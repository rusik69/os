/*
 * src/drivers/iscsi.c — iSCSI initiator driver.
 *
 * Connects to an iSCSI target over TCP (port 3260), performs the
 * full login phase (SecurityNegotiation → LoginOperationalNegotiation →
 * FullFeaturePhase), and maps the remote LUN as a local block device
 * via the blockdev framework.
 *
 * Simple implementation: no CHAP, no iSER, no MC/S, no digests.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "iscsi.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "blockdev.h"
#include "net.h"
#include "errno.h"

/* ── Global state ──────────────────────────────────────────────────── */

static struct iscsi_session g_sessions[ISCSI_MAX_DEVICES];
static int g_iscsi_initialized = 0;

/* ── Helpers: TCP send/recv ─────────────────────────────────────────── */

static int iscsi_tcp_send(int conn_id, const void *data, uint32_t len)
{
    if (!net_tcp_is_connected(conn_id))
        return -1;
    int sent = net_tcp_send(conn_id, data, len);
    return (sent == (int)len) ? 0 : -1;
}

static int iscsi_tcp_recv(int conn_id, void *buf, uint32_t len, int timeout_ticks)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t remaining = len;
    while (remaining > 0) {
        int n = net_tcp_recv(conn_id, p, remaining, timeout_ticks);
        if (n <= 0) return -1;
        p += n;
        remaining -= (uint32_t)n;
    }
    return 0;
}

/* ── Network byte order helpers (big-endian for iSCSI) ─────────────── */

static inline uint16_t iscsi_htons(uint16_t v) {
    return ((v >> 8) & 0xFF) | ((v << 8) & 0xFF00);
}

static inline uint32_t iscsi_htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000U);
}

static inline uint64_t iscsi_htonll(uint64_t v) {
    uint32_t hi = (uint32_t)(v >> 32);
    uint32_t lo = (uint32_t)(v & 0xFFFFFFFFULL);
    return ((uint64_t)iscsi_htonl(lo) << 32) | (uint64_t)iscsi_htonl(hi);
}

/* ── PDU transmit and receive ───────────────────────────────────────── */

static int iscsi_transmit_pdu(struct iscsi_session *sess,
                              struct iscsi_bhs *bhs,
                              const void *data_seg, uint32_t data_len)
{
    bhs->data_seg_len = iscsi_htonl(data_len);
    if (iscsi_tcp_send(sess->conn_id, bhs, sizeof(struct iscsi_bhs)) < 0)
        return -EIO;
    if (data_len > 0 && data_seg) {
        if (iscsi_tcp_send(sess->conn_id, data_seg, data_len) < 0)
            return -EIO;
    }
    return 0;
}

static int iscsi_receive_pdu(struct iscsi_session *sess,
                             struct iscsi_bhs *bhs_out,
                             void *data_buf, uint32_t *data_len_out)
{
    if (iscsi_tcp_recv(sess->conn_id, bhs_out, sizeof(struct iscsi_bhs), 100) < 0)
        return -EIO;

    uint32_t dlen = iscsi_htonl(bhs_out->data_seg_len);
    if (dlen > 0 && data_buf && dlen <= *data_len_out) {
        if (iscsi_tcp_recv(sess->conn_id, data_buf, dlen, 100) < 0)
            return -EIO;
    }
    if (data_len_out)
        *data_len_out = dlen;
    return 0;
}

/* ── Login phase ─────────────────────────────────────────────────────── */

static int iscsi_login(struct iscsi_session *sess)
{
    char param_buf[512];
    struct iscsi_bhs bhs;
    struct iscsi_bhs resp_bhs;
    char resp_buf[512];
    uint32_t resp_len;

    /* ── Stage 1: SecurityNegotiation (send login with no auth) ── */
    memset(&bhs, 0, sizeof(bhs));
    bhs.opcode = ISCSI_OP_LOGIN | ISCSI_OP_FINAL;
    bhs.flags  = ISCSI_LOGIN_STAGE_SECURITY_NEGOTIATION;  /* CSG */
    /* bhs.flags also contains the transit bit — we set it later in the update */

    /* Build parameter string: no auth required */
    int plen = snprintf(param_buf, sizeof(param_buf),
                        "InitiatorName=iqn.2026-06.kernel.iscsi:initiator\r\n"
                        "SessionType=Normal\r\n"
                        "MaxConnections=1\r\n"
                        "HeaderDigest=None\r\n"
                        "DataDigest=None\r\n"
                        "AuthMethod=None\r\n");

    if (iscsi_transmit_pdu(sess, &bhs, param_buf, (uint32_t)plen) < 0) {
        kprintf("[iscsi] Login stage 1 send failed\n");
        return -EIO;
    }

    /* Receive login response */
    resp_len = sizeof(resp_buf);
    if (iscsi_receive_pdu(sess, &resp_bhs, resp_buf, &resp_len) < 0) {
        kprintf("[iscsi] Login stage 1 recv failed\n");
        return -EIO;
    }

    kprintf("[iscsi] Login response: opcode=0x%02x flags=0x%02x dlen=%u\n",
            resp_bhs.opcode, resp_bhs.flags, iscsi_htonl(resp_bhs.data_seg_len));

    /* Check status (implied 0 = success) */
    if ((resp_bhs.flags & 0x3F) != 0) {
        kprintf("[iscsi] Login stage 1 failed (status class != 0)\n");
        return -EACCES;
    }

    /* ── Stage 2: LoginOperationalNegotiation ── */
    memset(&bhs, 0, sizeof(bhs));
    bhs.opcode = ISCSI_OP_LOGIN | ISCSI_OP_FINAL;
    /* Transition to FullFeaturePhase: TSG=Full, CSG=OperationalNegotiation, T=1 */
    bhs.flags = (1 << 6) |                             /* T bit (transit) */
                (ISCSI_LOGIN_STAGE_OP_NEGOTIATION << 2) | /* CSG */
                ISCSI_LOGIN_STAGE_FULL_FEATURE_PHASE;     /* NSG (lower 2 bits) */

    plen = snprintf(param_buf, sizeof(param_buf),
                    "InitiatorName=iqn.2026-06.kernel.iscsi:initiator\r\n"
                    "TargetName=%s\r\n"
                    "SessionType=Normal\r\n"
                    "HeaderDigest=None\r\n"
                    "DataDigest=None\r\n"
                    "MaxRecvDataSegmentLength=262144\r\n"
                    "MaxXmitDataSegmentLength=262144\r\n",
                    sess->target_name);

    if (iscsi_transmit_pdu(sess, &bhs, param_buf, (uint32_t)plen) < 0) {
        kprintf("[iscsi] Login stage 2 send failed\n");
        return -EIO;
    }

    resp_len = sizeof(resp_buf);
    if (iscsi_receive_pdu(sess, &resp_bhs, resp_buf, &resp_len) < 0) {
        kprintf("[iscsi] Login stage 2 recv failed\n");
        return -EIO;
    }

    /* Extract TSIH from the login response BHS */
    sess->tsih = iscsi_htons((uint16_t)(resp_bhs.lun >> 48));  /* TSIH is in bytes 6-7 of LUN field */

    kprintf("[iscsi] Login complete, TSIH=0x%04x\n", sess->tsih);
    sess->login_done = 1;
    return 0;
}

/* ── SCSI command helpers ────────────────────────────────────────────── */

static int iscsi_submit_scsi_cmd(struct iscsi_session *sess,
                                  const uint8_t *cdb, int cdb_len,
                                  void *data, int data_len,
                                  int data_dir)
{
    struct iscsi_bhs bhs;
    uint8_t pdu_buf[64];  /* BHS + small CDB */

    memset(&bhs, 0, sizeof(bhs));
    bhs.opcode = ISCSI_OP_SCSI_CMD | ISCSI_OP_FINAL;

    if (data_dir == ISCSI_DATA_IN)
        bhs.flags |= ISCSI_FLAG_READ;
    else if (data_dir == ISCSI_DATA_OUT)
        bhs.flags |= ISCSI_FLAG_WRITE;

    /* Fill in expected data length (big-endian) in the LUN/len field */
    /* For simplicity we use ITT as a tag */
    bhs.itt = iscsi_htonl(sess->cmd_sn++);
    bhs.exp_cmd_sn = iscsi_htonl(sess->cmd_sn);
    bhs.max_cmd_sn = iscsi_htonl(sess->cmd_sn + 1);

    /* Set data segment length to the CDB length (unsolicited data for writes) */
    if (data_dir == ISCSI_DATA_OUT) {
        bhs.data_seg_len = iscsi_htonl((uint32_t)data_len);
    }

    /* Send BHS */
    if (iscsi_transmit_pdu(sess, &bhs, NULL, 0) < 0)
        return -EIO;

    /* Send CDB as AHS (or inline if small) */
    if (cdb_len > 0) {
        if (iscsi_tcp_send(sess->conn_id, cdb, (uint32_t)cdb_len) < 0)
            return -EIO;
    }

    /* For writes: send data after the command */
    if (data_dir == ISCSI_DATA_OUT && data && data_len > 0) {
        if (iscsi_tcp_send(sess->conn_id, data, (uint32_t)data_len) < 0)
            return -EIO;
    }

    /* Receive response */
    struct iscsi_bhs rsp_bhs;
    uint32_t rsp_dlen = 0;
    if (iscsi_receive_pdu(sess, &rsp_bhs, NULL, &rsp_dlen) < 0)
        return -EIO;

    /* For reads: receive data */
    if (data_dir == ISCSI_DATA_IN && data && rsp_dlen > 0) {
        if (rsp_dlen > (uint32_t)data_len)
            rsp_dlen = (uint32_t)data_len;
        if (iscsi_tcp_recv(sess->conn_id, data, rsp_dlen, 100) < 0)
            return -EIO;
    }

    /* Check status */
    if (rsp_bhs.flags & 0x01) { /* Check Condition */
        kprintf("[iscsi] SCSI command failed (check condition)\n");
        return -EIO;
    }
    return 0;
}

/* ── INQUIRY ─────────────────────────────────────────────────────────── */

static int __attribute__((unused)) iscsi_inquiry(struct iscsi_session *sess, uint8_t *data, int max_len)
{
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_INQUIRY;
    cdb[3] = 0;  /* EVPD=0, page=0 */
    cdb[4] = (uint8_t)(max_len > 255 ? 255 : max_len);
    return iscsi_submit_scsi_cmd(sess, cdb, 6, data, max_len, ISCSI_DATA_IN);
}

/* ── READ CAPACITY (10) ──────────────────────────────────────────────── */

static int iscsi_read_capacity_10(struct iscsi_session *sess)
{
    uint8_t cdb[10];
    uint8_t data[8];
    int ret;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_READ_CAPACITY_10;
    cdb[8] = 0;  /* partial medium indicator */

    ret = iscsi_submit_scsi_cmd(sess, cdb, 10, data, 8, ISCSI_DATA_IN);
    if (ret < 0) return ret;

    /* READ CAPACITY(10) returns 8 bytes: 4 bytes LBA (last), 4 bytes block length */
    uint32_t last_lba = iscsi_htonl(*(uint32_t *)data);
    uint32_t block_len = iscsi_htonl(*(uint32_t *)(data + 4));

    sess->sector_count = (uint64_t)last_lba + 1;
    sess->sector_size = block_len;
    kprintf("[iscsi] Capacity: %llu sectors, %u bytes/sector\n",
            (unsigned long long)sess->sector_count, sess->sector_size);
    return 0;
}

/* ── Block device submit function ─────────────────────────────────────── */

static struct iscsi_session *iscsi_find_by_dev_id(int dev_id)
{
    for (int i = 0; i < ISCSI_MAX_DEVICES; i++) {
        if (g_sessions[i].connected && g_sessions[i].dev_id == dev_id)
            return &g_sessions[i];
    }
    return NULL;
}

static int iscsi_submit_fn(struct blk_request *req)
{
    if (!req) return -EINVAL;

    int dev_id = req->dev_id;
    struct iscsi_session *sess = iscsi_find_by_dev_id(dev_id);
    if (!sess || !sess->connected)
        return -ENODEV;

    int is_write = (req->flags & BLK_REQ_WRITE) ? 1 : 0;
    uint64_t lba = req->lba;
    uint32_t count = req->count;
    uint32_t byte_len = count * sess->sector_size;
    uint8_t cdb[10];
    int ret;

    memset(cdb, 0, sizeof(cdb));
    if (is_write) {
        cdb[0] = SCSI_OPCODE_WRITE_10;
        cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
        cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
        cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
        cdb[5] = (uint8_t)(lba & 0xFF);
        cdb[7] = (uint8_t)((count >> 8) & 0xFF);
        cdb[8] = (uint8_t)(count & 0xFF);
        ret = iscsi_submit_scsi_cmd(sess, cdb, 10, req->buf, (int)byte_len, ISCSI_DATA_OUT);
    } else {
        cdb[0] = SCSI_OPCODE_READ_10;
        cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
        cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
        cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
        cdb[5] = (uint8_t)(lba & 0xFF);
        cdb[7] = (uint8_t)((count >> 8) & 0xFF);
        cdb[8] = (uint8_t)(count & 0xFF);
        ret = iscsi_submit_scsi_cmd(sess, cdb, 10, req->buf, (int)byte_len, ISCSI_DATA_IN);
    }

    req->result = ret;
    return ret;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void iscsi_init(void)
{
    if (g_iscsi_initialized) return;
    memset(g_sessions, 0, sizeof(g_sessions));
    g_iscsi_initialized = 1;
    kprintf("[iscsi] iSCSI initiator subsystem initialized (max %d sessions)\n",
            ISCSI_MAX_DEVICES);
}

int iscsi_connect(uint32_t target_ip, const char *target_name)
{
    if (!g_iscsi_initialized) iscsi_init();

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < ISCSI_MAX_DEVICES; i++) {
        if (!g_sessions[i].connected) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        kprintf("[iscsi] No free session slots\n");
        return -1;
    }

    /* Connect TCP */
    int conn_id = net_tcp_connect(target_ip, ISCSI_PORT);
    if (conn_id < 0) {
        kprintf("[iscsi] TCP connect failed to %d.%d.%d.%d:%d\n",
                NIPQUAD(target_ip), ISCSI_PORT);
        return -1;
    }

    kprintf("[iscsi] Connected to %d.%d.%d.%d:%d (conn=%d)\n",
            NIPQUAD(target_ip), ISCSI_PORT, conn_id);

    /* Initialize session */
    struct iscsi_session *sess = &g_sessions[slot];
    memset(sess, 0, sizeof(*sess));
    sess->conn_id = conn_id;
    sess->target_ip = target_ip;
    sess->target_port = ISCSI_PORT;
    if (target_name)
        strncpy(sess->target_name, target_name, sizeof(sess->target_name) - 1);
    sess->isid = 0x400001370000ULL;  /* Generated ISID */

    /* Login */
    if (iscsi_login(sess) < 0) {
        net_tcp_close(conn_id);
        kprintf("[iscsi] Login failed\n");
        return -1;
    }

    /* Get capacity */
    if (iscsi_read_capacity_10(sess) < 0) {
        net_tcp_close(conn_id);
        kprintf("[iscsi] READ CAPACITY failed\n");
        return -1;
    }

    /* Register as block device */
    int iscsi_id = slot + 30;  /* Start iSCSI device IDs at 30 */
    sess->dev_id = iscsi_id;

    char name[16];
    snprintf(name, sizeof(name), "iscsi%d", slot);

    int ret = blockdev_register(iscsi_id, name,
                                 iscsi_submit_fn, NULL,
                                 sess->sector_count, 0);
    if (ret != 0) {
        kprintf("[iscsi] Failed to register block device %s\n", name);
        net_tcp_close(conn_id);
        memset(sess, 0, sizeof(*sess));
        return -1;
    }

    sess->connected = 1;
    kprintf("[iscsi] Device %s (id=%d): %llu sectors, %u bytes/sector\n",
            name, iscsi_id, (unsigned long long)sess->sector_count,
            sess->sector_size);
    return iscsi_id;
}

void iscsi_disconnect(int dev_id)
{
    struct iscsi_session *sess = iscsi_find_by_dev_id(dev_id);
    if (!sess) {
        kprintf("[iscsi] Device %d not found\n", dev_id);
        return;
    }

    blockdev_unregister(dev_id);
    net_tcp_close(sess->conn_id);
    kprintf("[iscsi] Device iscsi%d (id=%d) disconnected\n",
            (int)(sess - g_sessions), dev_id);
    memset(sess, 0, sizeof(*sess));
}
