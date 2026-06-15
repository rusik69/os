#ifndef FCOE_H
#define FCOE_H

#include "types.h"

/* ── Constants ─────────────────────────────────────────────────── */

/* FCoE Ethertype */
#define ETH_TYPE_FCOE            0x8906

/* FC frame delimiters */
#define FC_SOF_F                0x28  /* Fabric SOF */
#define FC_SOF_I2               0x2D  /* SOF i2 */
#define FC_SOF_N2               0x35  /* SOF n2 */
#define FC_SOF_I3               0x2E  /* SOF i3 */
#define FC_EOF_N                0x41  /* EOF normal */
#define FC_EOF_T                0x42  /* EOF terminate */
#define FC_EOF_A                0x44  /* EOF abort */

/* FC header fields */
#define FC_R_CTL_CMD            0x22  /* Extended Link Services */  /* Actually 0x22 for FLOGI */
#define FC_R_CTL_SCSI_CMD       0x32  /* SCSI FCP command (FC-4 data) */
#define FC_R_CTL_SCSI_DATA      0x01  /* SCSI FCP data */
#define FC_R_CTL_SCSI_STATUS    0x33  /* SCSI FCP status */
#define FC_R_CTL_SOL_DATA       0x01  /* Solicited Data */
#define FC_R_CTL_UNSOL_DATA     0x02  /* Unsolicited Data */
#define FC_R_CTL_FC4_DATA       0x01  /* FC-4 Data */
#define FC_R_CTL_XFER_RDY       0x05  /* Transfer Ready */

/* FC frame types */
#define FC_TYPE_BLS             0x00  /* Basic Link Service */
#define FC_TYPE_ELS             0x01  /* Extended Link Service */
#define FC_TYPE_FC4             0x04  /* FC-4 (SCSI-FCP) */

/* FC ELS codes */
#define FC_ELS_FLOGI            0x04  /* Fabric Login */
#define FC_ELS_PLOGI            0x05  /* Port Login */
#define FC_ELS_LOGO             0x07  /* Logout */
#define FC_ELS_ECHO             0x10  /* Echo */
#define FC_ELS_RPSC             0x18  /* Request Port State Change */
#define FC_ELS_SCR              0x22  /* State Change Registration */

/* FCP info types */
#define FCP_CMND                0x00
#define FCP_XFER_RDY            0x01
#define FCP_DATA                0x02
#define FCP_RSP                 0x03

/* Well-known addresses */
#define FC_FABRIC_D_ID          0xFFFFFE
#define FC_F_PORT_D_ID          0xFFFFFE  /* Fabric F-Port */

/* FCoE max devices */
#define FCOE_MAX_DEVICES        2

/* SCSI opcodes used by FCoE */
#define SCSI_OPCODE_INQUIRY          0x12
#define SCSI_OPCODE_READ_CAPACITY_10 0x25
#define SCSI_OPCODE_READ_10          0x28
#define SCSI_OPCODE_WRITE_10         0x2A

/* ── Structures ───────────────────────────────────────────────── */

/* FCoE frame (Ethernet + FCoE header) */
struct fcoe_frame {
    uint8_t  eth_dst[6];
    uint8_t  eth_src[6];
    uint16_t eth_type;       /* 0x8906 */
    uint8_t  fcoe_version;   /* 0x00 */
    uint8_t  fcoe_flags;     /* SOF in bits 3:0, optionally EOF */
    uint8_t  fcoe_reserved[6];
    /* FC frame follows (SOF, FC header, payload, CRC, EOF) */
} __attribute__((packed));

/* FC frame header (24 bytes) */
struct fc_header {
    uint8_t  r_ctl;          /* Routing Control */
    uint8_t  d_id[3];        /* Destination ID (24-bit) */
    uint8_t  _rsv0;
    uint8_t  s_id[3];        /* Source ID (24-bit) */
    uint8_t  type;           /* Data structure type */
    uint8_t  f_ctl[3];       /* Frame Control */
    uint8_t  seq_id;
    uint8_t  df_ctl;         /* Data Field Control */
    uint16_t seq_cnt;
    uint16_t ox_id;          /* Originator Exchange ID */
    uint16_t rx_id;          /* Responder Exchange ID */
    uint32_t parameter;      /* Parameter */
} __attribute__((packed));

/* SCSI FCP command descriptor */
struct fcp_cmnd {
    uint8_t  ref[4];         /* FCP_LUN */
    uint8_t  cn;             /* CRN/RSV */
    uint8_t  _rsv;
    uint8_t  pu;             /* FCP_PRIO_UA */
    uint8_t  _rsv2;
    uint8_t  cdb[16];        /* SCSI CDB */
    uint32_t fcp_dl;         /* Data length */
} __attribute__((packed));

/* FCP response */
struct fcp_rsp {
    uint8_t  _rsv[10];
    uint8_t  retry;
    uint8_t  _rsv2;
    uint16_t resid;
    uint32_t sense_len;
    uint32_t response_len;
    uint8_t  rsp_data[8];
    uint8_t  sense_data[32];
} __attribute__((packed));

/* FCoE device state */
struct fcoe_device {
    int      connected;
    int      dev_id;          /* block device ID */
    uint8_t  local_mac[6];
    uint8_t  target_mac[6];
    uint32_t s_id;            /* Source FC ID */
    uint32_t d_id;            /* Destination FC ID */
    uint16_t ox_id;           /* Exchange ID */
    uint32_t sector_count;
    uint32_t sector_size;
};

/* ── Public API ─────────────────────────────────────────────────── */

void fcoe_init(void);
int  fcoe_connect(void);
void fcoe_disconnect(int dev_id);
void fcoe_poll(void);

#endif /* FCOE_H */
