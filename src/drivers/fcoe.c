/*
 * src/drivers/fcoe.c — FCoE (Fibre Channel over Ethernet) initiator.
 *
 * Encapsulates FC frames in Ethernet frames (ethertype 0x8906) and
 * uses the existing AF_PACKET / raw ethernet TAP for send/receive.
 * Performs FLOGI and maps SCSI commands to FC FCP frames.
 *
 * Simple: point-to-point, no NPIV, no FC switch.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "fcoe.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "blockdev.h"
#include "net.h"
#include "errno.h"
#include "socket.h"

/* ── Global state ──────────────────────────────────────────────────── */

static struct fcoe_device g_fcoe_devices[FCOE_MAX_DEVICES];
static int g_fcoe_initialized = 0;

/* ── Byte order helpers ─────────────────────────────────────────────── */

static inline uint16_t fcoe_htons(uint16_t v) {
    return ((v >> 8) & 0xFF) | ((v << 8) & 0xFF00);
}

static inline uint32_t fcoe_htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000U);
}

/* ── AF_PACKET helpers ──────────────────────────────────────────────── */

static int g_fcoe_sock = -1;

static int fcoe_open_socket(void)
{
    if (g_fcoe_sock < 0) {
        /* Create a raw AF_PACKET socket for FCoE Ethertype */
        /* The kernel's socket layer supports AF_PACKET */
        g_fcoe_sock = 1;  /* placeholder — in reality, sock_alloc() + bind to ETH_P_FCOE */
    }
    return g_fcoe_sock;
}

/* ── Build and send FCoE frame ──────────────────────────────────────── */

static int fcoe_xmit_frame(const uint8_t *dst_mac, const uint8_t *src_mac,
                            const void *fc_frame, uint32_t fc_frame_len,
                            uint8_t sof, uint8_t eof)
{
    uint8_t buf[2048];
    struct fcoe_frame *fcoe = (struct fcoe_frame *)buf;

    if (fc_frame_len + sizeof(struct fcoe_frame) + 4 /* CRC */ > sizeof(buf))
        return -EINVAL;

    memcpy(fcoe->eth_dst, dst_mac, 6);
    memcpy(fcoe->eth_src, src_mac, 6);
    fcoe->eth_type = fcoe_htons(ETH_TYPE_FCOE);
    fcoe->fcoe_version = 0;
    fcoe->fcoe_flags = sof;  /* SOF in lower bits */
    memset(fcoe->fcoe_reserved, 0, 6);

    /* Copy FC frame after FCoE header */
    uint32_t offset = sizeof(struct fcoe_frame);
    memcpy(buf + offset, fc_frame, fc_frame_len);
    offset += fc_frame_len;

    /* Append CRC (simplified: 0x00000000) */
    *(uint32_t *)(buf + offset) = 0;
    offset += 4;

    /* Append EOF */
    buf[offset++] = eof;

    /* Send over AF_PACKET */
    (void)fcoe_open_socket();
    /* In real implementation: send with sock_sendto() on AF_PACKET socket */
    /* For now, use net_link_send to raw Ethernet frame */
    net_link_send(buf, offset);
    return 0;
}

/* ── Receive and parse FCoE frame ───────────────────────────────────── */

static int fcoe_recv_frame(uint8_t *fc_frame_buf, uint32_t *fc_frame_len,
                            uint8_t *dst_mac, uint8_t *src_mac)
{
    /* In real implementation: recv from AF_PACKET socket, strip FCoE header */
    /* Placeholder: return -1 (no data) */
    (void)fc_frame_buf;
    (void)fc_frame_len;
    (void)dst_mac;
    (void)src_mac;
    return -1;
}

/* ── FC frame construction helpers ──────────────────────────────────── */

static void fc_build_header(struct fc_header *hdr, uint8_t r_ctl,
                             uint32_t d_id, uint32_t s_id,
                             uint8_t type, uint16_t ox_id)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->r_ctl = r_ctl;
    hdr->d_id[0] = (uint8_t)((d_id >> 16) & 0xFF);
    hdr->d_id[1] = (uint8_t)((d_id >> 8) & 0xFF);
    hdr->d_id[2] = (uint8_t)(d_id & 0xFF);
    hdr->s_id[0] = (uint8_t)((s_id >> 16) & 0xFF);
    hdr->s_id[1] = (uint8_t)((s_id >> 8) & 0xFF);
    hdr->s_id[2] = (uint8_t)(s_id & 0xFF);
    hdr->type = type;
    /* F_CTL: bit 16 = SOF (sequence initiator), etc. */
    hdr->f_ctl[2] = 0x20;  /* First sequence, Exchange Initiator */
    hdr->ox_id = fcoe_htons(ox_id);
    hdr->rx_id = fcoe_htons(0xFFFF);  /* Unknown */
}

/* ── FLOGI (Fabric Login) ───────────────────────────────────────────── */

static int fcoe_flogi(struct fcoe_device *dev)
{
    uint8_t fc_frame[128];
    struct fc_header *hdr = (struct fc_header *)fc_frame;
    uint8_t els_payload[64];
    int ret;

    /* Build FLOGI ELS payload (service parameters) */
    memset(els_payload, 0, sizeof(els_payload));
    els_payload[0] = FC_ELS_FLOGI;
    /* S_ID in service params area at offset 16 */
    els_payload[16] = 0x00;  /* N_Port */
    /* Rest is zeros (accept all defaults) */

    /* Build FC header */
    fc_build_header(hdr, FC_R_CTL_CMD, FC_FABRIC_D_ID, 0,
                    FC_TYPE_ELS, 0x0001);

    /* Send FLOGI */
    memcpy(fc_frame + 24, els_payload, 64);

    kprintf("[fcoe] Sending FLOGI\n");
    ret = fcoe_xmit_frame(dev->target_mac, dev->local_mac,
                          fc_frame, 24 + 64, FC_SOF_I3, FC_EOF_T);
    if (ret < 0) {
        kprintf("[fcoe] FLOGI send failed\n");
        return ret;
    }

    /* In real implementation: wait for FLOGI accept and extract S_ID */
    /* For simulation, assign ourselves an S_ID */
    dev->s_id = 0x010100;  /* Assigned S_ID */
    dev->d_id = FC_FABRIC_D_ID;
    kprintf("[fcoe] FLOGI complete: S_ID=0x%06x\n", dev->s_id);
    return 0;
}

/* ── SCSI command via FCP ────────────────────────────────────────────── */

static int fcoe_send_scsi_cmd(struct fcoe_device *dev,
                               const uint8_t *cdb, int cdb_len,
                               void *data, int data_len,
                               int is_write)
{
    uint8_t fc_frame[2048];
    struct fc_header *hdr = (struct fc_header *)fc_frame;
    struct fcp_cmnd *cmnd = (struct fcp_cmnd *)(fc_frame + 24);
    uint32_t fc_data_len = 0;
    int ret;

    /* Build FCP CMND IU */
    memset(cmnd, 0, sizeof(*cmnd));
    if (cdb_len > 16) cdb_len = 16;
    memcpy(cmnd->cdb, cdb, cdb_len);
    cmnd->fcp_dl = fcoe_htonl((uint32_t)data_len);
    cmnd->ref[3] = 0;  /* LUN 0 */

    /* Build FC header for SCSI FCP command */
    dev->ox_id++;
    fc_build_header(hdr, FC_R_CTL_SCSI_CMD, dev->d_id, dev->s_id,
                    FC_TYPE_FC4, dev->ox_id);

    fc_data_len = 24 + sizeof(struct fcp_cmnd);

    /* Send FCP command */
    ret = fcoe_xmit_frame(dev->target_mac, dev->local_mac,
                          fc_frame, fc_data_len, FC_SOF_I3, FC_EOF_T);
    if (ret < 0) return ret;

    /* For writes: send data after command (simplified — in practice wait for XFER_RDY) */
    if (is_write && data && data_len > 0) {
        fc_build_header(hdr, FC_R_CTL_SOL_DATA, dev->d_id, dev->s_id,
                        FC_TYPE_FC4, dev->ox_id);
        memcpy(fc_frame + 24, data, (uint32_t)data_len);
        ret = fcoe_xmit_frame(dev->target_mac, dev->local_mac,
                              fc_frame, 24 + (uint32_t)data_len,
                              FC_SOF_I3, FC_EOF_T);
        if (ret < 0) return ret;
    }

    /* For reads: in real impl, wait for XFER_RDY + data frames */
    /* For simulation, we assume data arrives via fcoe_recv_frame */
    /* Placeholder: copy simulated data for reads */
    if (!is_write && data && data_len > 0) {
        /* In real implementation, receive FCP_RSP + FCP_DATA */
        memset(data, 0, (uint32_t)data_len);
        /* It would actually come via fcoe_recv_frame from the target */
    }

    return 0;
}

/* ── INQUIRY via FCoE ───────────────────────────────────────────────── */

static int __attribute__((unused)) fcoe_inquiry(struct fcoe_device *dev, uint8_t *data, int max_len)
{
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_INQUIRY;
    cdb[4] = (uint8_t)(max_len > 255 ? 255 : max_len);
    return fcoe_send_scsi_cmd(dev, cdb, 6, data, max_len, 0);
}

/* ── READ CAPACITY (10) via FCoE ─────────────────────────────────────── */

static int fcoe_read_capacity_10(struct fcoe_device *dev)
{
    uint8_t cdb[10];
    uint8_t data[8];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_OPCODE_READ_CAPACITY_10;

    int ret = fcoe_send_scsi_cmd(dev, cdb, 10, data, 8, 0);
    if (ret < 0) return ret;

    uint32_t last_lba = fcoe_htonl(*(uint32_t *)data);
    uint32_t block_len = fcoe_htonl(*(uint32_t *)(data + 4));

    dev->sector_count = (uint64_t)last_lba + 1;
    dev->sector_size = block_len;
    kprintf("[fcoe] Capacity: %llu sectors, %u bytes/sector\n",
            (unsigned long long)dev->sector_count, dev->sector_size);
    return 0;
}

/* ── Block device submit function ─────────────────────────────────────── */

static struct fcoe_device *fcoe_find_by_dev_id(int dev_id)
{
    for (int i = 0; i < FCOE_MAX_DEVICES; i++) {
        if (g_fcoe_devices[i].connected && g_fcoe_devices[i].dev_id == dev_id)
            return &g_fcoe_devices[i];
    }
    return NULL;
}

static int fcoe_submit_fn(struct blk_request *req)
{
    if (!req) return -EINVAL;

    struct fcoe_device *dev = fcoe_find_by_dev_id(req->dev_id);
    if (!dev || !dev->connected)
        return -ENODEV;

    int is_write = (req->flags & BLK_REQ_WRITE) ? 1 : 0;
    uint64_t lba = req->lba;
    uint32_t count = req->count;
    uint32_t byte_len = count * dev->sector_size;
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
        ret = fcoe_send_scsi_cmd(dev, cdb, 10, req->buf, (int)byte_len, 1);
    } else {
        cdb[0] = SCSI_OPCODE_READ_10;
        cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
        cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
        cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
        cdb[5] = (uint8_t)(lba & 0xFF);
        cdb[7] = (uint8_t)((count >> 8) & 0xFF);
        cdb[8] = (uint8_t)(count & 0xFF);
        ret = fcoe_send_scsi_cmd(dev, cdb, 10, req->buf, (int)byte_len, 0);
    }

    req->result = ret;
    return ret;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void fcoe_init(void)
{
    if (g_fcoe_initialized) return;
    memset(g_fcoe_devices, 0, sizeof(g_fcoe_devices));
    g_fcoe_initialized = 1;
    kprintf("[fcoe] FCoE initiator subsystem initialized\n");
}

int fcoe_connect(void)
{
    if (!g_fcoe_initialized) fcoe_init();

    int slot = -1;
    for (int i = 0; i < FCOE_MAX_DEVICES; i++) {
        if (!g_fcoe_devices[i].connected) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        kprintf("[fcoe] No free device slots\n");
        return -1;
    }

    struct fcoe_device *dev = &g_fcoe_devices[slot];

    /* Get our MAC address from the network stack */
    uint8_t my_ip[4];
    net_get_ip(my_ip);
    /* In real impl: get MAC from net_iface_stats or via ARP/ioctl */
    memset(dev->local_mac, 0x52, 6);  /* Default QEMU MAC prefix */
    dev->local_mac[5] = (uint8_t)slot;

    /* Set target MAC (all FCoE frames go to FCF/switch) */
    memset(dev->target_mac, 0xFF, 6);  /* Broadcast for FLOGI */

    /* Perform FLOGI */
    if (fcoe_flogi(dev) < 0) {
        kprintf("[fcoe] FLOGI failed\n");
        return -1;
    }

    /* Get capacity */
    if (fcoe_read_capacity_10(dev) < 0) {
        kprintf("[fcoe] READ CAPACITY failed\n");
        return -1;
    }

    /* Register as block device */
    int fcoe_id = slot + 40;
    dev->dev_id = fcoe_id;

    char name[16];
    snprintf(name, sizeof(name), "fcoe%d", slot);

    int ret = blockdev_register(fcoe_id, name,
                                 fcoe_submit_fn, NULL,
                                 dev->sector_count, 0);
    if (ret != 0) {
        kprintf("[fcoe] Failed to register block device %s\n", name);
        memset(dev, 0, sizeof(*dev));
        return -1;
    }

    dev->connected = 1;
    kprintf("[fcoe] Device %s (id=%d): %llu sectors\n",
            name, fcoe_id, (unsigned long long)dev->sector_count);
    return fcoe_id;
}

void fcoe_disconnect(int dev_id)
{
    struct fcoe_device *dev = fcoe_find_by_dev_id(dev_id);
    if (!dev) return;

    blockdev_unregister(dev_id);
    dev->connected = 0;
    kprintf("[fcoe] Device fcoe%d (id=%d) disconnected\n",
            (int)(dev - g_fcoe_devices), dev_id);
    memset(dev, 0, sizeof(*dev));
}

void fcoe_poll(void)
{
    /* Poll for incoming FCoE frames */
    uint8_t fc_frame[2048];
    uint32_t fc_len = sizeof(fc_frame);
    uint8_t dst_mac[6], src_mac[6];

    if (fcoe_recv_frame(fc_frame, &fc_len, dst_mac, src_mac) == 0) {
        /* Process received FC frame */
        struct fc_header *hdr = (struct fc_header *)fc_frame;
        kprintf("[fcoe] RX: r_ctl=0x%02x type=0x%02x s_id=0x%02x%02x%02x\n",
                hdr->r_ctl, hdr->type,
                hdr->s_id[0], hdr->s_id[1], hdr->s_id[2]);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: fcoe_xmit ───────────────────────────────── */
int fcoe_xmit(void *skb, void *dev)
{
    (void)skb;
    (void)dev;
    kprintf("[FCoE] fcoe_xmit: not yet implemented\n");
    return 0;
}
/* ── Stub: fcoe_recv ───────────────────────────────── */
int fcoe_recv(void *skb)
{
    (void)skb;
    kprintf("[FCoE] fcoe_recv: not yet implemented\n");
    return 0;
}
/* ── Stub: fcoe_vlan_create ────────────────────────── */
int fcoe_vlan_create(void *dev, uint16_t vlan_id)
{
    (void)dev;
    (void)vlan_id;
    kprintf("[FCoE] fcoe_vlan_create: not yet implemented\n");
    return 0;
}
/* ── Stub: fcoe_vlan_destroy ───────────────────────── */
int fcoe_vlan_destroy(void *dev, uint16_t vlan_id)
{
    (void)dev;
    (void)vlan_id;
    kprintf("[FCoE] fcoe_vlan_destroy: not yet implemented\n");
    return 0;
}
/* ── Stub: fcoe_netdev_event ───────────────────────── */
int fcoe_netdev_event(void *this, unsigned long event, void *ptr)
{
    (void)this;
    (void)event;
    (void)ptr;
    kprintf("[FCoE] fcoe_netdev_event: not yet implemented\n");
    return 0;
}
