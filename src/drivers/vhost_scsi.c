/*
 * src/drivers/vhost_scsi.c — In-kernel vhost SCSI target
 *
 * Exposes a SCSI LUN to the guest via virtio-scsi protocol.
 * Processes READ10, WRITE10, INQUIRY, TEST_UNIT_READY, REPORT_LUNS.
 * Backed by a local memory buffer (block device).
 */

#include "vhost_scsi.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"

/* ── Internal state ────────────────────────────────────────────────── */

#define VHOST_SCSI_MAX_LUNS 8

static int vhost_scsi_initialized = 0;
static struct vhost_scsi_lun vhost_scsi_luns[VHOST_SCSI_MAX_LUNS];
static int vhost_scsi_num_luns = 0;

/* ── SCSI command helpers ──────────────────────────────────────────── */

/* Build standard INQUIRY data */
static void scsi_build_inquiry(struct scsi_inquiry_data *inq,
                               struct vhost_scsi_lun *lun)
{
    memset(inq, 0, sizeof(*inq));
    inq->peripheral      = SCSI_INQ_PERIPH_DIRECT;
    inq->rmb             = 0x00;
    inq->version         = 0x06; /* SPC-4 */
    inq->response_data   = SCSI_INQ_RESPONSE_DATA;
    inq->additional_length = 0x1F; /* 31 bytes */
    inq->flags[0]        = 0x00;
    inq->flags[1]        = 0x00;
    inq->cmd_queue       = 0x02; /* CmdQue=1, multi-port? */
    memcpy(inq->vendor,   lun->vendor,   8);
    memcpy(inq->product,  lun->product,  16);
    memcpy(inq->revision, lun->revision, 4);
}

/* Build READ CAPACITY(10) data */
static void scsi_build_capacity10(struct scsi_capacity_data *cap,
                                  struct vhost_scsi_lun *lun)
{
    uint32_t last_lba = (lun->num_blocks > 0) ?
                         (uint32_t)(lun->num_blocks - 1) : 0;
    cap->lba        = __builtin_bswap32(last_lba);
    cap->block_size = __builtin_bswap32(VHOST_SCSI_SECTOR_SIZE);
}

/* Build REPORT LUNS data */
static void scsi_build_report_luns(struct scsi_report_luns_data *rld,
                                   int num_luns)
{
    memset(rld, 0, sizeof(*rld));
    uint32_t list_len = (uint32_t)num_luns * 8;
    rld->lun_list_length = __builtin_bswap32(list_len);
    for (int i = 0; i < num_luns && i < 1; i++) {
        /* LUN 0: flat space (bus 0, target 0, lun 0) */
        rld->luns[i] = __builtin_bswap64(0ULL);
    }
}

/* ── Handle a SCSI command ─────────────────────────────────────────── */

int vhost_scsi_handle_cmd(struct virtio_scsi_cmd_req *req,
                           struct virtio_scsi_cmd_resp *resp,
                           uint8_t *data_buf, uint32_t data_len)
{
    if (!req || !resp) return -1;

    uint8_t *cdb = req->cdb;
    uint8_t opcode = cdb[0];

    /* LUN selection (simplified: always LUN 0) */
    struct vhost_scsi_lun *lun = NULL;
    if (vhost_scsi_num_luns > 0)
        lun = &vhost_scsi_luns[0];

    memset(resp, 0, sizeof(*resp));
    resp->response = 0; /* OK */

    switch (opcode) {
    case SCSI_OP_TEST_UNIT_READY:
        /* Always ready */
        resp->status = VHOST_SCSI_GOOD;
        kprintf("[vhost-scsi] TEST_UNIT_READY\n");
        return 0;

    case SCSI_OP_INQUIRY: {
        if (!lun) {
            resp->status = VHOST_SCSI_CHECK_COND;
            return -1;
        }
        uint32_t alloc_len = (cdb[3] << 8) | cdb[4];
        uint32_t inq_len = sizeof(struct scsi_inquiry_data);
        uint32_t copy_len = (alloc_len < inq_len) ? alloc_len : inq_len;

        struct scsi_inquiry_data inq;
        scsi_build_inquiry(&inq, lun);
        if (data_buf && copy_len > 0)
            memcpy(data_buf, &inq, copy_len);
        resp->status = VHOST_SCSI_GOOD;
        kprintf("[vhost-scsi] INQUIRY: alloc_len=%u\n", alloc_len);
        return 0;
    }

    case SCSI_OP_READ_CAPACITY10: {
        if (!lun) {
            resp->status = VHOST_SCSI_CHECK_COND;
            return -1;
        }
        struct scsi_capacity_data cap;
        scsi_build_capacity10(&cap, lun);
        if (data_buf && data_len >= sizeof(cap))
            memcpy(data_buf, &cap, sizeof(cap));
        resp->status = VHOST_SCSI_GOOD;
        kprintf("[vhost-scsi] READ_CAPACITY10: blocks=%llu\n",
                lun->num_blocks);
        return 0;
    }

    case SCSI_OP_READ10: {
        if (!lun) {
            resp->status = VHOST_SCSI_CHECK_COND;
            return -1;
        }
        uint32_t lba = (cdb[2] << 24) | (cdb[3] << 16) |
                       (cdb[4] << 8)  | cdb[5];
        uint32_t num_blocks = (cdb[7] << 8) | cdb[8];
        if (num_blocks == 0) num_blocks = 256;

        uint64_t offset = (uint64_t)lba * VHOST_SCSI_SECTOR_SIZE;
        uint64_t length = (uint64_t)num_blocks * VHOST_SCSI_SECTOR_SIZE;

        if (offset + length > lun->num_blocks * VHOST_SCSI_SECTOR_SIZE) {
            resp->status = VHOST_SCSI_CHECK_COND;
            return -1;
        }
        if (data_buf && length <= data_len) {
            memcpy(data_buf, lun->data + offset, (size_t)length);
        }
        resp->status = VHOST_SCSI_GOOD;
        kprintf("[vhost-scsi] READ10: lba=%u blocks=%u\n", lba, num_blocks);
        return 0;
    }

    case SCSI_OP_WRITE10: {
        if (!lun || lun->readonly) {
            resp->status = VHOST_SCSI_CHECK_COND;
            return -1;
        }
        uint32_t lba = (cdb[2] << 24) | (cdb[3] << 16) |
                       (cdb[4] << 8)  | cdb[5];
        uint32_t num_blocks = (cdb[7] << 8) | cdb[8];
        if (num_blocks == 0) num_blocks = 256;

        uint64_t offset = (uint64_t)lba * VHOST_SCSI_SECTOR_SIZE;
        uint64_t length = (uint64_t)num_blocks * VHOST_SCSI_SECTOR_SIZE;

        if (offset + length > lun->num_blocks * VHOST_SCSI_SECTOR_SIZE) {
            resp->status = VHOST_SCSI_CHECK_COND;
            return -1;
        }
        if (data_buf && length <= data_len) {
            memcpy(lun->data + offset, data_buf, (size_t)length);
        }
        resp->status = VHOST_SCSI_GOOD;
        kprintf("[vhost-scsi] WRITE10: lba=%u blocks=%u\n", lba, num_blocks);
        return 0;
    }

    case SCSI_OP_REPORT_LUNS: {
        if (!lun) {
            resp->status = VHOST_SCSI_CHECK_COND;
            return -1;
        }
        struct scsi_report_luns_data rld;
        scsi_build_report_luns(&rld, vhost_scsi_num_luns);
        uint32_t alloc_len = (cdb[6] << 24) | (cdb[7] << 16) |
                             (cdb[8] << 8)  | cdb[9];
        uint32_t rld_len = sizeof(rld);
        uint32_t copy_len = (alloc_len < rld_len) ? alloc_len : rld_len;
        if (data_buf && copy_len > 0)
            memcpy(data_buf, &rld, copy_len);
        resp->status = VHOST_SCSI_GOOD;
        kprintf("[vhost-scsi] REPORT_LUNS: alloc_len=%u\n", alloc_len);
        return 0;
    }

    case SCSI_OP_MODE_SENSE6: {
        /* Return short mode sense — no mode pages */
        if (data_buf && data_len >= 4) {
            data_buf[0] = 0; /* mode data length */
            data_buf[1] = 0; /* medium type */
            data_buf[2] = 0; /* device-specific parameter */
            data_buf[3] = 0; /* block descriptor length */
        }
        resp->status = VHOST_SCSI_GOOD;
        kprintf("[vhost-scsi] MODE_SENSE6\n");
        return 0;
    }

    case SCSI_OP_REQUEST_SENSE: {
        /* Return no sense data */
        if (data_buf && data_len >= 18) {
            memset(data_buf, 0, 18);
        }
        resp->status = VHOST_SCSI_GOOD;
        kprintf("[vhost-scsi] REQUEST_SENSE\n");
        return 0;
    }

    default:
        kprintf("[vhost-scsi] Unsupported opcode 0x%02x\n", opcode);
        resp->status = VHOST_SCSI_CHECK_COND;
        resp->response = 1; /* command unsupported */
        return -1;
    }
}

/* ── Virtqueue kick handler ────────────────────────────────────────── */

/* Process a kick from the guest on a virtqueue.
 * In a real implementation this would:
 *  1. Read descriptors from the virtqueue
 *  2. Extract the SCSI command request
 *  3. Call vhost_scsi_handle_cmd()
 *  4. Write back the response
 *  5. Update the used ring
 */
int vhost_scsi_handle_kick(int vq_idx)
{
    if (!vhost_scsi_initialized) return -1;

    kprintf("[vhost-scsi] kick on virtqueue %d\n", vq_idx);

    /* In a real vhost implementation, we'd process the virtqueue here.
     * For this reference implementation, we log the kick.
     *
     * Simplified flow:
     *   1. Read avail ring
     *   2. For each available descriptor:
     *      a. Read scatter-gather list
     *      b. First descriptor = request header
     *      c. Second descriptor (if WRITE) = data from guest
     *      d. Third descriptor = status byte
     *   3. Call vhost_scsi_handle_cmd()
     *   4. Write response and status
     *   5. Update used ring
     */
    return 0;
}

/* ── LUN management ────────────────────────────────────────────────── */

int vhost_scsi_add_lun(struct vhost_scsi_lun *lun)
{
    if (vhost_scsi_num_luns >= VHOST_SCSI_MAX_LUNS)
        return -1;

    if (!lun || !lun->data) return -1;

    memcpy(&vhost_scsi_luns[vhost_scsi_num_luns], lun,
           sizeof(struct vhost_scsi_lun));
    vhost_scsi_num_luns++;

    kprintf("[vhost-scsi] LUN %d added: %llu blocks, readonly=%d\n",
            vhost_scsi_num_luns - 1,
            lun->num_blocks, lun->readonly);
    return 0;
}

/* ── Cleanup ───────────────────────────────────────────────────────── */

void vhost_scsi_cleanup(void)
{
    memset(vhost_scsi_luns, 0, sizeof(vhost_scsi_luns));
    vhost_scsi_num_luns = 0;
    vhost_scsi_initialized = 0;
    kprintf("[vhost-scsi] cleaned up\n");
}

/* ── Init ──────────────────────────────────────────────────────────── */

int vhost_scsi_init(void)
{
    memset(vhost_scsi_luns, 0, sizeof(vhost_scsi_luns));
    vhost_scsi_num_luns = 0;

    /* Create default LUN 0 (16 MB, writable, memory-backed) */
    uint64_t num_pages = (16ULL * 1024 * 1024) / PAGE_SIZE;
    uint64_t lun_mem = (uint64_t)pmm_alloc_frames((size_t)num_pages);
    if (!lun_mem) {
        kprintf("[vhost-scsi] Failed to allocate backing storage\n");
        return -1;
    }

    struct vhost_scsi_lun default_lun;
    memset(&default_lun, 0, sizeof(default_lun));
    default_lun.data       = (uint8_t *)PHYS_TO_VIRT(lun_mem);
    default_lun.num_blocks = (16ULL * 1024 * 1024) / 512;
    default_lun.readonly   = 0;
    memcpy(default_lun.vendor,   "HERMES", 6);
    memcpy(default_lun.product,  "vHost SCSI Disk ", 16);
    memcpy(default_lun.revision, "1.0 ", 4);

    if (vhost_scsi_add_lun(&default_lun) < 0) {
        kprintf("[vhost-scsi] Failed to add default LUN\n");
        return -1;
    }

    vhost_scsi_initialized = 1;
    kprintf("[vhost-scsi] vhost SCSI target initialized: "
            "%d LUN(s)\n", vhost_scsi_num_luns);
    return 0;
}
#include "module.h"
module_init(vhost_scsi_init);

/* ── Stub: vhost_scsi_start ─────────────────────────────── */
int vhost_scsi_start(void *dev)
{
    (void)dev;
    kprintf("[vhost] vhost_scsi_start: not yet implemented\n");
    return 0;
}
/* ── Stub: vhost_scsi_stop ─────────────────────────────── */
int vhost_scsi_stop(void *dev)
{
    (void)dev;
    kprintf("[vhost] vhost_scsi_stop: not yet implemented\n");
    return 0;
}
