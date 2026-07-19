#ifndef NVME_H
#define NVME_H

#include "types.h"
#include "cpu_bitmask.h"

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
#define NVME_CC_ENABLE    (1U << 0)
#define NVME_CC_CSS_NVM   (0 << 4)
#define NVME_CC_MPS_SHIFT 7
#define NVME_CC_AMS_RR    (0 << 11)
#define NVME_CC_SHN_NORMAL (1U << 14)
#define NVME_CC_SHN_ABRUPT (3 << 14)
#define NVME_CC_IOSQES    6    /* SQ entry size = 2^6 = 64 bytes */
#define NVME_CC_IOCQES    4    /* CQ entry size = 2^4 = 16 bytes */

/* Controller Status bits */
#define NVME_CSTS_RDY     (1U << 0)
#define NVME_CSTS_CFS     (1U << 1)
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
#define NVME_ADMIN_SANITIZE    0x44

/* I/O commands */
#define NVME_IO_WRITE     0x01
#define NVME_IO_READ      0x02
#define NVME_IO_DATASET_MANAGEMENT 0x09  /* Deallocate (TRIM) */

/* Identify CNS values */
#define NVME_IDENTIFY_NS   0x00
#define NVME_IDENTIFY_CTRL 0x01

/* Feature identifiers */
#define NVME_FEAT_NUMBER_OF_QUEUES 0x07
#define NVME_FEAT_PREDICTABLE_LATENCY_CONFIG 0x09
#define NVME_FEAT_PREDICTABLE_LATENCY_WINDOW 0x0A

/* Queue configuration */
#define NVME_IO_QUEUE_SIZE   64   /* entries per I/O SQ/CQ */
#define NVME_IO_QUEUE_MAX    16   /* max I/O queue pairs (bounded by CPUMASK_MAX_CPUS) */

/* Max namespace count we support */
#define NVME_MAX_NS          8

/* Per-namespace block device info */
#define NVME_BLOCKDEV_ID     8   /* starting dev_id for NVMe namespaces */

/* Doorbell stride: SQ doorbell at 0x1000 + (2*qid)*stride, */
/* CQ doorbell at 0x1000 + (2*qid+1)*stride, stride = 4 << dstrd */

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

/* Identify namespace data (4096 bytes) */
struct nvme_identify_ns {
    uint64_t nsze;     /* Namespace size in LBA's */
    uint64_t ncap;     /* Namespace capacity */
    uint64_t nuse;     /* Namespace utilization */
    uint8_t  nsfeat;
    uint8_t  nlbaf;    /* Number of LBA formats - 1 */
    uint8_t  flbas;
    uint8_t  mc;
    uint8_t  dpc;
    uint8_t  dps;
    uint8_t  nmic;
    uint8_t  rescap;
    uint8_t  fpi;
    uint8_t  dlfeat;
    uint16_t nawun;
    uint16_t nawupf;
    uint16_t nacwu;
    uint16_t nabsn;
    uint16_t nabo;
    uint16_t nabspf;
    uint16_t noiob;
    uint8_t  _rsv[80];
    uint64_t lbbaf[];
} __attribute__((packed));

/* LBA format data */
struct nvme_lba_format {
    uint16_t ms;
    uint8_t  ds;       /* Data block size = 2^ds */
    uint8_t  rp;
};

/* ── NVMe Power Management (PS Profiles) ──────────────────────────── */

/* Maximum number of power state descriptors per NVMe spec */
#define NVME_MAX_POWER_STATES     32

/* Power management feature identifiers */
#define NVME_FEAT_POWER_MANAGEMENT              0x02
#define NVME_FEAT_AUTO_POWER_STATE_TRANSITION   0x0C

/* Offsets in Identify Controller data (4096-byte buffer) */
#define NVME_ID_CTRL_NPSS_OFFSET        512   /* Number of Power States Supported (u8) */
#define NVME_ID_CTRL_PSDESC_OFFSET      968   /* Power State Descriptors start at 0x3C8 */

/* Power State Descriptor (32 bytes, one per supported power state).
 * Array starts at offset 0x3C8 in Identify Controller data, 32 entries max. */
struct nvme_power_state_desc {
    uint16_t mp;              /* Maximum Power (centiwatts = 0.01 W).
                               * Bit 15 = Non-Operational State (NOS). */
    uint16_t reserved0;
    uint32_t enlat;           /* Entry Latency (microseconds). 0xFFFFFFFF = unspecified. */
    uint32_t exlat;           /* Exit Latency (microseconds).  0xFFFFFFFF = unspecified. */
    uint8_t  rrt;             /* Relative Read Throughput (0 = best) */
    uint8_t  rrl;             /* Relative Read Latency   (0 = best) */
    uint8_t  rwt;             /* Relative Write Throughput (0 = best) */
    uint8_t  rwl;             /* Relative Write Latency   (0 = best) */
    uint16_t idle_power;      /* Bits [15:6] Idle Power (centiwatts or watts)
                               * Bit  5    Idle Scale (0: centiwatts, 1: watts)
                               * Bits [4:0] Idle Time (100 ms units) */
    uint8_t  active_power_scale; /* Bits [6:5] AP Scale
                                  *   (0: centiwatts, 1: watts)
                                  * Bits [4:0] AP Time (100 ms units) */
    uint8_t  active_power;       /* Bits [7]   AP Workload hint
                                  * Bits [6:0] Active Power */
    uint16_t reserved1;
    uint8_t  reserved2[4];
} __attribute__((packed));

/* Autonomous Power State Transition (APST) entry (8 bytes) */
struct nvme_apst_entry {
    uint32_t idle_time;       /* Idle time prior to transition (ms).
                               * 0 = no timeout transition from this condition. */
    uint32_t idle_ps;         /* Power state to transition to (bits [4:0]).
                               * Upper bits reserved. */
} __attribute__((packed));

/* Per-CPU I/O queue pair */
struct nvme_io_queue {
    uint8_t   valid;
    uint8_t   cpu_id;      /* CPU this queue belongs to */
    uint16_t  qid;         /* Queue ID (also the admin queue's CQ ID) */

    /* Submission queue */
    void     *sq_virt;
    uint64_t  sq_phys;
    uint16_t  sq_tail;
    uint16_t  sq_size;

    /* Completion queue */
    void     *cq_virt;
    uint64_t  cq_phys;
    uint16_t  cq_head;
    uint16_t  cq_size;

    /* Expected phase tag (P bit in status field, bit 0) for CQ completions.
     * Initialised to 1 after controller enable.  Toggled when the CQ head
     * wraps, matching the controller's phase tag toggle. */
    uint8_t   cq_phase;

    /* Doorbell stride (cached from CAP) */
    uint32_t  stride;

    /* IRQ vector */
    int       irq_vector;
};

/* NVMe controller state */
struct nvme_ctrl {
    int      present;
    int      initialized;
    uint64_t regs;        /* MMIO register base (virtual) */
    uint64_t phys_regs;   /* Physical register base */
    int      irq;
    uint32_t max_q_depth;
    uint32_t nn;          /* Number of namespaces */
    uint32_t sq_entry_size;
    uint32_t cq_entry_size;

    /* Admin queues */
    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    void    *admin_sq;
    void    *admin_cq;
    uint64_t admin_sq_phys;
    uint64_t admin_cq_phys;

    /* Expected phase tag (P bit) for the admin completion queue.
     * Initialised to 1, toggled on CQ wrap. */
    uint8_t  admin_cq_phase;

    /* I/O queues */
    uint32_t  nr_io_queues;       /* Number of I/O queue pairs created */
    uint32_t  doorbell_stride;    /* Stride in bytes between doorbells */
    struct nvme_io_queue io_queues[NVME_IO_QUEUE_MAX];

    /* Namespace block device IDs */
    int      ns_blkdev_id[NVME_MAX_NS];
    uint64_t ns_sector_count[NVME_MAX_NS];
    uint32_t ns_sector_size[NVME_MAX_NS];

    /* Controller capabilities (Item 328: bio splitting) */
    uint8_t  mdts;           /* Maximum Data Transfer Size (log2 units of MPS) */
    uint8_t  mpsmin;         /* Minimum Memory Page Size (exponent, base 4096 = 0) */

    /* Power state descriptors (cached from Identify Controller) */
    uint8_t  npss;                                    /* Number of Power States Supported */
    int      power_states_parsed;                      /* Non-zero once parsed */
    struct nvme_power_state_desc power_states[32];     /* PS descriptors (max 32 per spec) */
};

/* API */
int  nvme_init(void);
int  nvme_is_present(void);
int  nvme_identify_ctrl(struct nvme_identify_ctrl *id);
int  nvme_submit_admin_cmd(struct nvme_sq_entry *cmd, struct nvme_cq_entry *cqe);
void nvme_print_info(void);
int  nvme_sanitize(int action, int overwrite_pass_count);

/* Sanitize action codes */
#define NVME_SANITIZE_ACTION_BLOCK_ERASE    1
#define NVME_SANITIZE_ACTION_OVERWRITE      2
#define NVME_SANITIZE_ACTION_CRYPTO_ERASE   3

/* Sanitize status feature (FID 0x81) */
#define NVME_FEAT_SANITIZE_STATUS           0x81

/* Sanitize status values (SSTAT byte) */
#define NVME_SANITIZE_STATUS_NEVER          0x00  /* Never been sanitized */
#define NVME_SANITIZE_STATUS_COMPLETE       0x01  /* Sanitize completed */
#define NVME_SANITIZE_STATUS_IN_PROGRESS    0x02  /* Sanitize in progress */
#define NVME_SANITIZE_STATUS_FAILED         0x03  /* Sanitize failed */
#define NVME_SANITIZE_STATUS_FRMT           0x10  /* Sanitize via Format NVM */
#define NVME_SANITIZE_STATUS_FRMT_IN_PROG   0x11  /* Format NVM in progress */
#define NVME_SANITIZE_STATUS_NEVER_NC       0x20  /* Never sanitized (no-cleanup) */

/* Sanitize status data returned by Get Features (FID 0x81) */
struct nvme_sanitize_status {
    uint16_t progress;          /* SCPROG: 0–65535 completion progress */
    uint8_t  reserved1[6];
    uint32_t estimated_time;    /* Estimated time to complete sanitize (seconds) */
    uint32_t estimated_time_er; /* Estimated time after reset/restart (seconds) */
    uint8_t  reserved2[16];
} __attribute__((packed));

/* Query the sanitize status of the controller via Get Features (FID 0x81).
 * Fills in the provided nvme_sanitize_status structure with progress and
 * status information.  Returns 0 on success, negative errno on failure. */
int  nvme_sanitize_get_status(struct nvme_sanitize_status *status);

/* ── NVMe Power Management API (PS Profiles) ──────────────────────── */

/* Get the number of power states supported by the controller.
 * Returns 0 if power state info is not yet available (identify not run). */
int  nvme_get_power_state_count(void);

/* Get a power state descriptor by index (0..npss-1).
 * Returns 0 on success, -EINVAL if index out of range. */
int  nvme_get_power_state_desc(int ps_index, struct nvme_power_state_desc *desc);

/* Transition the controller to a specific power state via Set Features (FID 0x02).
 * @ps_index: power state number to transition to (0..npss-1).
 * Returns 0 on success, negative errno on failure. */
int  nvme_set_power_state(int ps_index);

/* Get the current power state via Get Features (FID 0x02).
 * Returns the current PS number (0..npss-1) on success, negative errno on failure. */
int  nvme_get_current_power_state(void);

/* Enable or disable Autonomous Power State Transition (APST, FID 0x0C).
 * When enabling, an APST table with @nr_entries is provided.  Pass @table=NULL
 * and @nr_entries=0 to disable APST.
 * Returns 0 on success, negative errno on failure. */
int  nvme_set_apst(int enable, struct nvme_apst_entry *table, int nr_entries);

/* Print all power state descriptors to the console for diagnostics. */
void nvme_print_power_states(void);

/* I/O queue submit function (called from blockdev layer) */
int nvme_submit_request(int ns_id, int is_write, uint64_t lba,
                        uint64_t count, void *buf);

/* Dataset Management — deallocate (TRIM) a range of LBAs on a namespace.
 * Returns 0 on success, -1 on failure. */
int nvme_deallocate(int ns_id, uint64_t lba, uint32_t count);

/* ── NVMe Asynchronous Event Request (AER) ────────────────────────── */

/* Asynchronous Event types (completion cdw0 bits [7:0]) */
#define NVME_AER_TYPE_ERROR                 0x01
#define NVME_AER_TYPE_SMART                 0x02
#define NVME_AER_TYPE_CMDSET                0x03
#define NVME_AER_TYPE_VENDOR                0x04

/* Error event information (for NVME_AER_TYPE_ERROR) */
#define NVME_AER_ERR_SQ                     0x00
#define NVME_AER_ERR_INVALID_DB             0x01
#define NVME_AER_ERR_DIAG_FAIL              0x02
#define NVME_AER_ERR_PERSIST_INTERNAL       0x03
#define NVME_AER_ERR_VENDOR_SPECIFIC        0x04

/* SMART / Health event information (for NVME_AER_TYPE_SMART) */
#define NVME_AER_SMART_RELIABILITY          0x00
#define NVME_AER_SMART_TEMP_THRESH          0x01
#define NVME_AER_SMART_SPARE_THRESH         0x02

/* AER functions — init/poll/exit for continuous async event monitoring */
int  nvme_aer_init(void);
void nvme_aer_poll(void);
void nvme_aer_exit(void);

/* ── NVMe Multipath API ────────────────────────────────────────────── */

/* Per-path statistics (exported for sysfs/debugfs) */
struct nvme_path_stats {
    uint64_t success_count;
    uint64_t fail_count;
    uint64_t total_latency_ticks;
    uint64_t last_latency_ticks;
};

/* Get per-path statistics for a multipath device.
 * Returns 0 on success, -1 if device not found. */
int nvme_mpath_get_stats(int mp_dev_id,
                          int *nr_paths,
                          struct nvme_path_stats *stats_out,
                          int max_paths);

/* ── NVMe Predictable Latency Mode (FID 0x09, 0x0A) ────────────────── */

/* DTYPE values for Predictable Latency Mode Config (FID 0x09, cdw11 bits [1:0]) */
#define NVME_PLM_DTYPE_DISABLED         0x0  /* Predictable latency mode disabled */
#define NVME_PLM_DTYPE_DETERMINISTIC    0x1  /* Deterministic (bounded latency) window */
#define NVME_PLM_DTYPE_NON_DETERMINISTIC 0x2 /* Non-deterministic window */

/* Window types returned by Predictable Latency Mode Window (FID 0x0A, cdw0 bits [1:0]) */
#define NVME_PLM_WINDOW_NONE            0x0  /* No window active */
#define NVME_PLM_WINDOW_DETERMINISTIC   0x1  /* Deterministic window active */
#define NVME_PLM_WINDOW_NON_DETERMINISTIC 0x2 /* Non-deterministic window active */

/* Set/Get the predictable latency mode configuration (FID 0x09).
 * @dtype: NVME_PLM_DTYPE_DISABLED (0), NVME_PLM_DTYPE_DETERMINISTIC (1), or
 *         NVME_PLM_DTYPE_NON_DETERMINISTIC (2).
 * Returns 0 on success, negative errno on failure. */
int nvme_set_predictable_latency(int dtype);

/* Get the current predictable latency mode configuration (FID 0x09).
 * Returns the current DTYPE value (0=disabled, 1=deterministic, 2=non-deterministic)
 * on success, or negative errno on failure. */
int nvme_get_predictable_latency(void);

/* Get the current predictable latency window status (FID 0x0A).
 * Returns the window type (NVME_PLM_WINDOW_*) on success,
 * or negative errno on failure. */
int nvme_get_predictable_latency_window(void);

/* Print predictable latency mode status for diagnostics. */
void nvme_print_predictable_latency(void);

/* ── NVMe PMR (Persistent Memory Region) API ─────────────────────── */

/* Initialise PMR subsystem.  Returns 0 on success, negative errno. */
int nvme_pmr_init(void);

/* Read from PMR at byte offset into buf. */
int nvme_pmr_read(uint64_t offset, void *buf, uint64_t len);

/* Write to PMR at byte offset from buf. */
int nvme_pmr_write(uint64_t offset, const void *buf, uint64_t len);

/* Returns non-zero if PMR is present and enabled. */
int nvme_pmr_is_present(void);

/* Returns PMR size in bytes, or 0 if not present. */
uint64_t nvme_pmr_get_size(void);

/* Returns kernel virtual address of PMR mapping, or NULL. */
void *nvme_pmr_get_virt(void);

/* Shut down PMR and unmap resources. */
void nvme_pmr_exit(void);

#endif /* NVME_H */
