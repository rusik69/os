#ifndef NVMF_H
#define NVMF_H

#include "types.h"
#include "nvme.h"

/* ── Constants ─────────────────────────────────────────────────── */

#define NVMF_PORT                4420
#define NVMF_MAX_CONNECTIONS     1

/* NVMe-oF PDU types (capsule, response, etc.) */
#define NVMF_PDU_IC_REQ          0x01  /* Connect - Initial Cmd capsule */
#define NVMF_PDU_IC_RSP          0x02  /* Connect response */
#define NVMF_PDU_CAPSULE_CMD     0x03  /* Capsule command */
#define NVMF_PDU_CAPSULE_RSP     0x04  /* Capsule response */
#define NVMF_PDU_H2C_DATA        0x05  /* Host-to-Controller data */
#define NVMF_PDU_C2H_DATA        0x06  /* Controller-to-Host data */
#define NVMF_PDU_R2T             0x07  /* Ready to Transfer */
#define NVMF_PDU_PROP_SET        0x08  /* Property Set */
#define NVMF_PDU_PROP_GET        0x09  /* Property Get */
#define NVMF_PDU_PROP_SET_RSP    0x0A  /* Property Set Response */
#define NVMF_PDU_PROP_GET_RSP    0x0B  /* Property Get Response */

/* NVMe-oF version */
#define NVMF_VERSION_MAJOR       1
#define NVMF_VERSION_MINOR       0

/* Allowed host NQN (simplified: any) */
#define NVMF_HOST_NQN            "nqn.2026-06.kernel:nvmf_target"

/* Fabrics command opcodes */
#define NVMF_FABRIC_COMMAND      0x7F  /* NVMe Admin cmd with Fuse=11b */
#define NVMF_FABRIC_CONNECT      0x00
#define NVMF_FABRIC_PROPERTY_SET 0x01
#define NVMF_FABRIC_PROPERTY_GET 0x02

/* ── Structures ───────────────────────────────────────────────── */

/* NVMe-oF common PDU header (16 bytes) */
struct nvmf_pdu_hdr {
    uint8_t  type;           /* NVMF_PDU_* */
    uint8_t  flags;
    uint16_t len;            /* total PDU length (big-endian) */
    uint32_t cid;            /* Command ID */
    uint32_t tag;            /* Tag for response matching */
} __attribute__((packed));

/* NVMe-oF IC_REQ (Initial Command Request) */
struct nvmf_ic_req {
    struct nvmf_pdu_hdr hdr;
    uint16_t recfmt;         /* RECFMT (big-endian) */
    uint16_t maxr2t;         /* Max R2T count */
    uint32_t hpsnt;          /* Max host-to-controller PDU size */
    uint32_t dpsnt;          /* Max controller-to-host PDU size */
    uint32_t hpda;           /* Max host PDU data alignment */
    uint32_t hrat;           /* Host response aggregation timeout */
    uint32_t hras;           /* Host response aggregation size */
    uint8_t  _rsv[20];
} __attribute__((packed));

/* NVMe-oF IC_RSP (Connect Response) */
struct nvmf_ic_rsp {
    struct nvmf_pdu_hdr hdr;
    uint16_t recfmt;
    uint16_t crto;           /* Controller Response Timeout */
    uint32_t maxr2t;
    uint32_t dpsnt;
    uint32_t cpda;           /* Controller PDU data alignment */
    uint32_t crat;           /* Controller response aggregation timeout */
    uint32_t cdas;           /* Controller data aggregation size */
    uint8_t  _rsv[20];
} __attribute__((packed));

/* NVMe-oF Capsule Command PDU (header for NVMe commands over Fabrics) */
struct nvmf_capsule_cmd {
    struct nvmf_pdu_hdr hdr;
    uint32_t nvme_cdw0;      /* NVMe command DWORD 0 */
    uint32_t nsid;
    uint64_t _rsv;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10_15[6];
} __attribute__((packed));

/* NVMe-oF Capsule Response PDU */
struct nvmf_capsule_rsp {
    struct nvmf_pdu_hdr hdr;
    struct nvme_cq_entry cqe;
} __attribute__((packed));

/* NVMe-oF C2H Data PDU */
struct nvmf_c2h_data {
    struct nvmf_pdu_hdr hdr;
    uint16_t cid;
    uint16_t _rsv;
    uint32_t datao;          /* data offset */
    uint32_t datal;          /* data length */
    /* data payload follows */
} __attribute__((packed));

/* NVMe-oF H2C Data PDU */
struct nvmf_h2c_data {
    struct nvmf_pdu_hdr hdr;
    uint16_t cid;
    uint16_t _rsv;
    uint32_t datao;
    uint32_t datal;
    /* data payload follows */
} __attribute__((packed));

/* NVMe-oF R2T PDU */
struct nvmf_r2t {
    struct nvmf_pdu_hdr hdr;
    uint16_t cid;
    uint16_t _rsv;
    uint32_t datao;
    uint32_t datal;
} __attribute__((packed));

/* NVMe-oF Property Set PDU */
struct nvmf_prop_set {
    struct nvmf_pdu_hdr hdr;
    uint32_t offset;
    uint32_t value;
    uint8_t  _rsv[8];
} __attribute__((packed));

/* NVMe-oF Property Get PDU */
struct nvmf_prop_get {
    struct nvmf_pdu_hdr hdr;
    uint32_t offset;
    uint8_t  _rsv[12];
} __attribute__((packed));

/* NVMe-oF Property Get Response */
struct nvmf_prop_get_rsp {
    struct nvmf_pdu_hdr hdr;
    uint32_t offset;
    uint32_t value;
    uint8_t  _rsv[8];
} __attribute__((packed));

/* Fabrics CONNECT command data (SGL, actually sent as data segment) */
struct nvmf_connect_cmd {
    uint8_t  opcode;         /* NVMF_FABRIC_CONNECT */
    uint8_t  _rsv0;
    uint16_t cid;
    uint32_t nsid;
    uint64_t _rsv1;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;          /* RECFMT (bits 31:16) + QID (bits 15:0) */
    uint32_t cdw11;          /* Host NQN offset? */
} __attribute__((packed));

/* Fabrics CONNECT response */
struct nvmf_connect_rsp {
    uint32_t cdw0;           /* status */
    uint32_t cdw1;           /* NQN offset if needed */
} __attribute__((packed));

/* ── NVMe-oF target state ─────────────────────────────────────────-- */

struct nvmf_target {
    int      active;
    int      listen_fd;      /* socket FD for listening */
    int      conn_id;        /* accepted TCP connection ID */
    int      connected;
    uint16_t port;

    /* Associated NVMe controller reference */
    int      nvme_nsid;      /* Namespace ID being exported */
    int      nvme_blkdev_id; /* Block device ID of the NVMe namespace */
};

/* ── Public API ─────────────────────────────────────────────────── */

void nvmf_init(void);
int  nvmf_start(uint16_t port, int nsid);
void nvmf_stop(void);
void nvmf_poll(void);

#endif /* NVMF_H */
