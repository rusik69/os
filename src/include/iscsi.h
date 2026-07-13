#ifndef ISCSI_H
#define ISCSI_H

#include "types.h"

/* ── Constants ─────────────────────────────────────────────────── */

#define ISCSI_PORT                3260
#define ISCSI_MAX_DEVICES         4

/* iSCSI BHS opcodes */
#define ISCSI_OP_LOGIN            0x03
#define ISCSI_OP_SCSI_CMD         0x01
#define ISCSI_OP_SCSI_RSP         0x21
#define ISCSI_OP_LOGIN_RSP        0x23
#define ISCSI_OP_TEXT             0x04
#define ISCSI_OP_TEXT_RSP         0x24
#define ISCSI_OP_LOGOUT           0x06
#define ISCSI_OP_LOGOUT_RSP       0x26
#define ISCSI_OP_NOP_OUT          0x00
#define ISCSI_OP_NOP_IN           0x20

/* iSCSI opcode direction bit (bit 5 = 0 for request, 1 for response) */
#define ISCSI_OP_IMMEDIATE        0x40
#define ISCSI_OP_FINAL            0x80

/* Login stage codes */
#define ISCSI_LOGIN_STAGE_SECURITY_NEGOTIATION      0
#define ISCSI_LOGIN_STAGE_OP_NEGOTIATION            1
#define ISCSI_LOGIN_STAGE_FULL_FEATURE_PHASE        3

/* SCSI opcodes used by iSCSI */
#define SCSI_OPCODE_TEST_UNIT_READY  0x00
#define SCSI_OPCODE_INQUIRY          0x12
#define SCSI_OPCODE_READ_CAPACITY_10 0x25
#define SCSI_OPCODE_READ_10          0x28
#define SCSI_OPCODE_WRITE_10         0x2A

/* SCSI direction */
#define ISCSI_DATA_NONE   0
#define ISCSI_DATA_IN     1  /* read: target → initiator */
#define ISCSI_DATA_OUT    2  /* write: initiator → target */

/* iSCSI PDU flags */
#define ISCSI_FLAG_READ   0x40
#define ISCSI_FLAG_WRITE  0x20
#define ISCSI_FLAG_FINAL  0x80

/* ── Structures ───────────────────────────────────────────────── */

/* iSCSI Basic Header Segment (BHS) — 48 bytes */
struct iscsi_bhs {
    uint8_t  opcode;       /* bits 7:0 — includes I (immediate) + F (final) */
    uint8_t  flags;        /* opcode-specific flags */
    uint16_t total_ahs_len;    /* total AHS length in 4-byte words */
    uint32_t data_seg_len;     /* data segment length in bytes */
    uint64_t lun;              /* LUN (8 bytes) */
    uint32_t itt;              /* Initiator Task Tag */
    uint32_t ttt;              /* Target Task Tag (for writes/commands) */
    uint32_t stat_sn;          /* Status SN */
    uint32_t exp_cmd_sn;       /* Expected Command SN */
    uint32_t max_cmd_sn;       /* Maximum Command SN */
    uint8_t  _rsv[3];
    uint8_t  hslen;            /* Header digest length (0 if no digest) */
    uint8_t  _bhs_pad[8];      /* BHS is 48 bytes per RFC 3720 */
} __attribute__((packed));

/* SCSI command descriptor block (CDB) — 16 bytes */
struct iscsi_scsi_cdb {
    uint8_t  opcode;
    uint8_t  bm_flags;     /* byte 1: bit 0=DPO, bit 1=FUA */
    uint32_t lba;          /* logical block address (big-endian) */
    uint8_t  group_num;
    uint16_t alloc_len;    /* for READ: allocation length */
    uint8_t  control;
    uint8_t  _pad[7];
} __attribute__((packed));

/* iSCSI login request PDU (opcode 0x03) */
struct iscsi_login_req {
    struct iscsi_bhs bhs;
    /* Parameters as text key=value pairs in data segment */
} __attribute__((packed));

/* iSCSI login response PDU (opcode 0x23) */
struct iscsi_login_rsp {
    struct iscsi_bhs bhs;
    /* Status class + detail in data segment */
} __attribute__((packed));

/* iSCSI SCSI command PDU */
struct iscsi_scsi_cmd_pdu {
    struct iscsi_bhs bhs;
    struct iscsi_scsi_cdb cdb;
    /* AHS and data segment follow */
} __attribute__((packed));

/* iSCSI SCSI response PDU */
struct iscsi_scsi_rsp_pdu {
    struct iscsi_bhs bhs;
    uint8_t  residual_count[4];
    uint8_t  sns_len;
    uint8_t  rsp_len;
    uint32_t max_burst;
    /* sense/response data follows */
} __attribute__((packed));

/* Per-device iSCSI session state */
struct iscsi_session {
    int      connected;
    int      conn_id;         /* TCP connection ID */
    char     target_name[224];
    uint32_t target_ip;
    uint16_t target_port;
    int      dev_id;          /* block device ID */

    /* Session identifiers */
    uint64_t isid;            /* Initiator Session ID (6 bytes in spec, stored as 64-bit) */
    uint16_t tsih;            /* Target Session Handle */

    /* Command sequencing */
    uint32_t cmd_sn;          /* Command Sequence Number */
    uint32_t stat_sn;         /* Status Sequence Number */
    uint32_t exp_stat_sn;     /* Expected Status SN */

    /* Device capacity */
    uint64_t sector_count;
    uint32_t sector_size;

    /* Login phase tracking */
    int      login_done;
};

/* ── Public API ─────────────────────────────────────────────────── */

void iscsi_init(void);
int  iscsi_connect(uint32_t target_ip, const char *target_name);
void iscsi_disconnect(int dev_id);
int  iscsi_read_capacity(struct iscsi_session *sess);

#endif /* ISCSI_H */
