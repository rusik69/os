#ifndef VHOST_SCSI_H
#define VHOST_SCSI_H

#include "types.h"

/* ── vhost-scsi constants ──────────────────────────────────────────── */
#define VHOST_SCSI_MAX_SECTORS    256
#define VHOST_SCSI_CDB_SIZE       32
#define VHOST_SCSI_SENSE_SIZE     96
#define VHOST_SCSI_SECTOR_SIZE    512
#define VHOST_SCSI_MAX_TARGET     1
#define VHOST_SCSI_MAX_LUN        8

/* SCSI command result codes */
#define VHOST_SCSI_GOOD           0x00
#define VHOST_SCSI_CHECK_COND     0x02
#define VHOST_SCSI_BUSY           0x08
#define VHOST_SCSI_RESERVATION    0x0C
#define VHOST_SCSI_TASK_SET_FULL  0x28
#define VHOST_SCSI_ACA_ACTIVE     0x30

/* SCSI opcodes */
#define SCSI_OP_TEST_UNIT_READY   0x00
#define SCSI_OP_INQUIRY           0x12
#define SCSI_OP_READ10            0x28
#define SCSI_OP_WRITE10           0x2A
#define SCSI_OP_READ_CAPACITY10   0x25
#define SCSI_OP_REPORT_LUNS       0xA0
#define SCSI_OP_MODE_SENSE6       0x1A
#define SCSI_OP_REQUEST_SENSE     0x03

/* INQUIRY data response codes */
#define SCSI_INQ_PERIPH_DIRECT    0x00
#define SCSI_INQ_RESPONSE_DATA    0x02  /* SPC-3 compliant */

/* ── SCSI command request/response (virtio-scsi wire format) ────────── */
#pragma pack(push, 1)
struct virtio_scsi_cmd_req {
    uint8_t  lun[8];
    uint64_t tag;
    uint8_t  task_attr;
    uint8_t  prio;
    uint8_t  crn;
    uint8_t  cdb[32];
};

struct virtio_scsi_cmd_resp {
    uint32_t sense_len;
    uint32_t residual;
    uint16_t status_qualifier;
    uint8_t  status;
    uint8_t  response;
    uint8_t  sense[96];
};
#pragma pack(pop)

/* ── SCSI INQUIRY data format ──────────────────────────────────────── */
#pragma pack(push, 1)
struct scsi_inquiry_data {
    uint8_t  peripheral;         /* bit 7:5 = qualifier, bit 4:0 = device type */
    uint8_t  rmb;                /* bit 7 = RMB, bits 6:0 reserved */
    uint8_t  version;            /* SCSI version */
    uint8_t  response_data;      /* response data format */
    uint8_t  additional_length;  /* n-4 */
    uint8_t  flags[2];
    uint8_t  cmd_queue;
    char     vendor[8];
    char     product[16];
    char     revision[4];
};

struct scsi_capacity_data {
    uint32_t lba;               /* last LBA */
    uint32_t block_size;        /* bytes per block */
};
#pragma pack(pop)

/* ── SCSI REPORT LUNS data ──────────────────────────────────────────── */
#pragma pack(push, 1)
struct scsi_report_luns_data {
    uint32_t lun_list_length;
    uint32_t reserved;
    uint64_t luns[1];           /* at least one */
};
#pragma pack(pop)

/* ── Backing store for vhost-scsi LUN ───────────────────────────────── */
struct vhost_scsi_lun {
    uint8_t      *data;         /* pointer to block data */
    uint64_t      num_blocks;   /* number of 512-byte blocks */
    int           readonly;
    char          vendor[8];
    char          product[16];
    char          revision[4];
};

/* ── Public API ─────────────────────────────────────────────────────── */
int vhost_scsi_init(void);
int vhost_scsi_add_lun(struct vhost_scsi_lun *lun);
int vhost_scsi_handle_kick(int vq_idx);
int vhost_scsi_handle_cmd(struct virtio_scsi_cmd_req *req,
                          struct virtio_scsi_cmd_resp *resp,
                          uint8_t *data_buf, uint32_t data_len);
void vhost_scsi_cleanup(void);

#endif /* VHOST_SCSI_H */
