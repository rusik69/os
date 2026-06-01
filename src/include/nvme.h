#ifndef NVME_H
#define NVME_H

#include "types.h"

/* NVMe PCI class code */
#define NVME_PCI_CLASS    0x01
#define NVME_PCI_SUBCLASS 0x08
#define NVME_PCI_PROG_IF  0x02

/* NVMe registers (BAR0, offset) */
#define NVME_REG_CAP      0x0000  /* Controller Capabilities */
#define NVME_REG_VS       0x0008  /* Version */
#define NVME_REG_INTMS    0x000C  /* Interrupt Mask Set */
#define NVME_REG_INTMC    0x0010  /* Interrupt Mask Clear */
#define NVME_REG_CC       0x0014  /* Controller Configuration */
#define NVME_REG_CSTS     0x001C  /* Controller Status */
#define NVME_REG_NSSR     0x0020  /* NVM Subsystem Reset */
#define NVME_REG_AQA      0x0024  /* Admin Queue Attributes */
#define NVME_REG_ASQ      0x0028  /* Admin Submission Queue Base */
#define NVME_REG_ACQ      0x0030  /* Admin Completion Queue Base */

/* Controller Configuration bits */
#define NVME_CC_ENABLE    (1 << 0)
#define NVME_CC_CSS_NVM   (0 << 4)
#define NVME_CC_MPS_SHIFT 7
#define NVME_CC_AMS_RR    (0 << 11)
#define NVME_CC_SHN_NORMAL (1 << 14)
#define NVME_CC_SHN_ABRUPT (3 << 14)
#define NVME_CC_IOSQES    6    /* SQ entry size = 2^6 = 64 bytes */
#define NVME_CC_IOCQES    4    /* CQ entry size = 2^4 = 16 bytes */

/* Controller Status bits */
#define NVME_CSTS_RDY     (1 << 0)
#define NVME_CSTS_CFS     (1 << 1)
#define NVME_CSTS_SHST_MASK (3 << 2)

/* Admin queue IDs */
#define NVME_ADMIN_SQ 0
#define NVME_ADMIN_CQ 0

/* Admin commands */
#define NVME_ADMIN_DELETE_SQ  0x00
#define NVME_ADMIN_CREATE_SQ  0x01
#define NVME_ADMIN_DELETE_CQ  0x04
#define NVME_ADMIN_CREATE_CQ  0x05
#define NVME_ADMIN_IDENTIFY   0x06
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_ADMIN_GET_FEATURES 0x0A
#define NVME_ADMIN_ASYNC_EVENT  0x0C

/* Identify CNS values */
#define NVME_IDENTIFY_NS   0x00
#define NVME_IDENTIFY_CTRL 0x01

/* Submission queue entry (64 bytes) */
struct nvme_sq_entry {
    uint32_t cdw0;    /* Command DWORD 0: opcode, fuse, etc */
    uint32_t nsid;    /* Namespace Identifier */
    uint64_t reserved;
    uint64_t mptr;    /* Metadata Pointer */
    uint64_t prp1;    /* Physical Region Page 1 */
    uint64_t prp2;    /* Physical Region Page 2 */
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

/* Completion queue entry (16 bytes) */
struct nvme_cq_entry {
    uint32_t cdw0;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed));

/* Identify controller data (4096 bytes) */
struct nvme_identify_ctrl {
    uint16_t vid;
    uint16_t ssvid;
    char     sn[20];
    char     mn[40];
    char     fr[8];
    uint8_t  rab;
    uint8_t  ieee[3];
    uint8_t  cmic;
    uint8_t  mdts;
    uint16_t cntlid;
    uint32_t ver;
    uint8_t  _res[180];
    uint32_t sqes;     /* bit 0-3: min SQ entry size (2^n), bit 4-7: max */
    uint32_t cqes;     /* bit 0-3: min CQ entry size (2^n), bit 4-7: max */
    uint32_t nn;       /* Number of namespaces */
    /* ... more fields ... */
} __attribute__((packed));

/* NVMe controller state */
struct nvme_ctrl {
    int      present;
    uint64_t regs;        /* MMIO register base (virtual) */
    uint64_t phys_regs;   /* Physical register base */
    int      irq;
    uint32_t max_q_depth;
    uint32_t nn;          /* Number of namespaces */
    uint32_t sq_entry_size;
    uint32_t cq_entry_size;
    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    void    *admin_sq;    /* Admin submission queue (virtual) */
    void    *admin_cq;    /* Admin completion queue (virtual) */
    uint64_t admin_sq_phys;
    uint64_t admin_cq_phys;
};

/* API */
int  nvme_init(void);
int  nvme_is_present(void);
int  nvme_identify_ctrl(struct nvme_identify_ctrl *id);
int  nvme_submit_admin_cmd(struct nvme_sq_entry *cmd, struct nvme_cq_entry *cqe);
void nvme_print_info(void);

#endif /* NVME_H */
