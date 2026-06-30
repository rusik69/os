#ifndef XHCI_H
#define XHCI_H

#include "types.h"

/* xHCI PCI class code: USB 3.0 xHCI */
#define XHCI_PCI_CLASS    0x0C
#define XHCI_PCI_SUBCLASS 0x03
#define XHCI_PCI_PROG_IF  0x30

/* xHCI capability registers (MMIO BAR0) */
#define XHCI_CAP_HCCAPBASE  0x00  /* CAPLENGTH + HCIVERSION */
#define XHCI_CAP_HCSPARAMS1 0x04  /* Structural Parameters 1 */
#define XHCI_CAP_HCSPARAMS2 0x08  /* Structural Parameters 2 */
#define XHCI_CAP_HCSPARAMS3 0x0C  /* Structural Parameters 3 */
#define XHCI_CAP_HCCPARAMS1 0x10  /* Capability Parameters 1 */
#define XHCI_CAP_DBOFF      0x14  /* Doorbell Offset */
#define XHCI_CAP_RTSOFF     0x18  /* Runtime Register Space Offset */

/* xHCI operational registers (base + CAPLENGTH) */
#define XHCI_OP_USBCMD      0x00  /* USB Command */
#define XHCI_OP_USBSTS      0x04  /* USB Status */
#define XHCI_OP_PAGESIZE    0x08  /* Page Size */
#define XHCI_OP_DNCTRL      0x14  /* Device Notification Control */
#define XHCI_OP_CRCR        0x18  /* Command Ring Control */
#define XHCI_OP_DCBAAP      0x30  /* Device Context Base Address Array */
#define XHCI_OP_CONFIG      0x38  /* Configure */

/* USBCMD bits */
#define XHCI_CMD_RUN        (1U << 0)
#define XHCI_CMD_HCRST      (1U << 1)
#define XHCI_CMD_INTE       (1U << 2)
#define XHCI_CMD_HSEE       (1U << 3)

/* USBSTS bits */
#define XHCI_STS_HCH        (1U << 0)
#define XHCI_STS_HSE        (1U << 2)
#define XHCI_STS_EINT       (1U << 3)
#define XHCI_STS_PCD        (1U << 4)
#define XHCI_STS_CNR        (1U << 11)

/* Port register set */
#define XHCI_PORTSC         0x400  /* Port Status and Control (per port, 0x10 each) */
#define XHCI_PORTPMSC       0x404  /* Port Power Management */
#define XHCI_PORTLI         0x408  /* Port Link Info */
#define XHCI_PORTHLPMC      0x40C  /* Port Hardware LPM Control */

/* PORTSC bits */
#define XHCI_PORTSC_CCS     (1U << 0)
#define XHCI_PORTSC_PED     (1U << 1)
#define XHCI_PORTSC_PR      (1U << 4)
#define XHCI_PORTSC_PP      (1U << 9)
#define XHCI_PORTSC_CSC     (1U << 16)
#define XHCI_PORTSC_PEC     (1U << 17)
#define XHCI_PORTSC_WRC     (1U << 19)
#define XHCI_PORTSC_OCC     (1U << 20)
#define XHCI_PORTSC_PRC     (1U << 21)
#define XHCI_PORTSC_PLC     (1U << 22)
#define XHCI_PORTSC_CEC     (1U << 23)
#define XHCI_PORTSC_WCE     (1U << 24)
#define XHCI_PORTSC_WDE     (1U << 25)

/* Port speed */
#define XHCI_PORTSC_PS_SHIFT 10
#define XHCI_PORTSC_PS_MASK  0x0F

/* Max ports */
#define XHCI_MAX_PORTS 16

/* ── TRB (Transfer Request Block) — 16 bytes ──────────────────────── */
struct xhci_trb {
    uint64_t parameter;    /* 0x00: data buffer pointer / setup packet / link ptr */
    uint32_t status;       /* 0x08: transfer length / completion code / TD size */
    uint32_t control;      /* 0x0C: cycle bit, TRB type, flags */
} __attribute__((packed, aligned(16)));

/* TRB type values (xHCI 1.2 §6.4.6) */
#define TRB_TYPE_NORMAL                    1
#define TRB_TYPE_SETUP                     2
#define TRB_TYPE_DATA                      3
#define TRB_TYPE_STATUS                    4
#define TRB_TYPE_ISOCH                     5
#define TRB_TYPE_LINK                      6
#define TRB_TYPE_EVENT_DATA                7
#define TRB_TYPE_NOOP                      8
#define TRB_TYPE_ENABLE_SLOT               9
#define TRB_TYPE_DISABLE_SLOT             10
#define TRB_TYPE_ADDRESS_DEVICE           11
#define TRB_TYPE_CONFIG_ENDPOINT          12
#define TRB_TYPE_EVAL_CONTEXT             13
#define TRB_TYPE_RESET_ENDPOINT           14
#define TRB_TYPE_STOP_ENDPOINT            15
#define TRB_TYPE_SET_TR_DEQUEUE           16
#define TRB_TYPE_RESET_DEVICE             17
#define TRB_TYPE_FORCE_EVENT              18
#define TRB_TYPE_NEGOTIATE_BANDWIDTH      19
#define TRB_TYPE_SET_LATENCY_TOLERANCE    20
#define TRB_TYPE_GET_PORT_BANDWIDTH       21
#define TRB_TYPE_FORCE_HEADER             22
#define TRB_TYPE_NOOP_COMMAND             23
#define TRB_TYPE_TRANSFER_EVENT           32
#define TRB_TYPE_COMMAND_COMPLETION       33
#define TRB_TYPE_PORT_STATUS_CHANGE       34
#define TRB_TYPE_BANDWIDTH_REQUEST        35
#define TRB_TYPE_DOORBELL                 36
#define TRB_TYPE_HOST_CONTROLLER_EVENT    37
#define TRB_TYPE_DEVICE_NOTIFICATION      38
#define TRB_TYPE_MFINDEX_WRAP             39

/* TRB control/status bit flags (xHCI 1.2 §4.11) */
#define TRB_CYCLE_BIT        (1U << 0)   /* Ownership: 1 = producer, 0 = consumer */
#define TRB_TC_BIT           (1U << 1)   /* Toggle Cycle  — Link TRB wraps consumer cycle */
#define TRB_CHAIN_BIT        (1U << 4)   /* Chain bit — continue TD */
#define TRB_IOC_BIT          (1U << 5)   /* Interrupt On Completion */
#define TRB_IDT_BIT          (1U << 6)   /* Immediate Data (data in TRB, not buffer) */
#define TRB_BSR_BIT          (1U << 9)   /* Block Set Address Request (Address Device cmd) */
#define TRB_TYPE_SHIFT       10
#define TRB_TYPE_MASK        0x3F
#define TRB_SET_TYPE(t)      (((uint32_t)(t) & TRB_TYPE_MASK) << TRB_TYPE_SHIFT)
#define TRB_GET_TYPE(ctl)    (((ctl) >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK)

/* TRB status: completion code extraction */
#define TRB_GET_COMP_CODE(sts)  ((int)((sts) & 0xFFU))

/* Transfer TRB: parameter field helpers */
#define TRB_XFER_LEN(len)    ((uint64_t)((len) & 0x1FFFFULL))
#define TRB_TD_SIZE(n)       (((uint64_t)((n) & 0x1F) << 17))
#define TRB_DIR_IN           (1ULL << 63)   /* Data stage direction for Data TRB */
#define TRB_XFER_LEN_GET(sts) ((int)((sts) & 0x1FFFFUL))

/* Setup Stage TRB: extract 8-byte setup packet from parameter field */
#define TRB_SETUP_TD_SIZE(n) (((uint32_t)((n) & 0x1F) << 17))
#define TRB_SETUP_DIR_IN     (1U << 16)

/* Isochronous TRB flags */
#define TRB_SIA_BIT          (1U << 31)

/* Link TRB: segment pointer (lower bits reserved) */
#define TRB_LINK_PTR(addr)   ((uint64_t)(addr) & ~0x0FULL)

/* ── Ring segment constants ──────────────────────────────────────── */
#define XHCI_RING_TRBS      256   /* 256 TRBs × 16 bytes = 4096 bytes = 1 page */
#define XHCI_RING_USABLE    (XHCI_RING_TRBS - 1)  /* last TRB is Link TRB */

/* Transfer / Command Ring */
struct xhci_ring {
    struct xhci_trb *trbs;       /* Virtual address of ring segment */
    uint64_t         paddr;      /* Physical address of ring segment */
    int              cycle;      /* Current producer cycle state (1=first pass) */
    int              enq_idx;    /* Producer index (next free slot 0..N-2) */
    int              deq_idx;    /* Consumer index (next TRB to consume) */
};

/* Event Ring Segment Table entry (16 bytes, 16-byte aligned) */
struct xhci_erst_entry {
    uint64_t seg_addr;    /* Bits 63:4 of ring segment base address */
    uint32_t seg_size;    /* Number of TRBs in this segment (bits 31:16) */
    uint32_t _rsvd;
} __attribute__((packed, aligned(16)));

/* Event Ring */
struct xhci_event_ring {
    struct xhci_trb   *trbs;          /* Virtual address of event ring segment */
    uint64_t           paddr;         /* Physical address */
    struct xhci_erst_entry *erst;     /* Virtual address of ERST array */
    uint64_t           erst_paddr;    /* Physical address of ERST array */
    int                erst_entries;  /* Number of ERST entries (1) */
    int                deq_idx;       /* Consumer index in event ring */
    int                cycle;         /* Consumer cycle state (initially 1) */
    int                num_trbs;      /* Number of TRBs in the event ring segment */
};

/* Event Ring registers in Runtime Register Space */
#define XHCI_ERSTSZ(base, n)  ((base) + 0x28 + (n) * 0x20)
#define XHCI_ERSTBA(base, n)  ((base) + 0x30 + (n) * 0x20)
#define XHCI_ERDP(base, n)    ((base) + 0x38 + (n) * 0x20)

/* Doorbell registers (Doorbell Offset + slot_id × 4) */
#define XHCI_DOORBELL_REG(db_off, slot) ((uint64_t)(db_off) + (uint64_t)(slot) * 4)

/* Completion code values (xHCI 1.2 §6.4.5) */
#define TRB_CC_SUCCESS                 1
#define TRB_CC_SHORT_PACKET           13
#define TRB_CC_STALL_ERROR            36
#define TRB_CC_BABBLE_ERROR           12
#define TRB_CC_TRB_ERROR               5
#define TRB_CC_SLOT_NOT_READY         33
#define TRB_CC_PARAMETER_ERROR         8
#define TRB_CC_CONTEXT_STATE_ERROR    19
#define TRB_CC_ENDPOINT_NOT_ON        28
#define TRB_CC_ENDPOINT_HALTED        26
#define TRB_CC_USB_TRANSACTION_ERROR   4

/* xHCI controller state */
struct xhci_controller {
    int      present;
    uint64_t cap_regs;       /* Capability registers base (virtual) */
    uint64_t op_regs;        /* Operational registers base */
    uint64_t db_off;         /* Doorbell offset */
    uint64_t rt_off;         /* Runtime offset */
    uint8_t  max_ports;
    uint8_t  max_slots;
    int      irq;
    uint32_t page_size;      /* Page size (1 << (page_size + 12)) */
    /* Ring infrastructure */
    struct xhci_ring     cmd_ring;      /* Command ring */
    struct xhci_event_ring ev_ring;     /* Event ring */
    int                  rings_initialized;
};

/* MMIO access helpers */
static inline uint32_t xhci_read32(struct xhci_controller *xhci, uint64_t base, uint64_t reg) {
    (void)xhci;
    return *(volatile uint32_t *)(uintptr_t)(base + reg);
}

static inline void xhci_write32(struct xhci_controller *xhci, uint64_t base, uint64_t reg, uint32_t val) {
    (void)xhci;
    *(volatile uint32_t *)(uintptr_t)(base + reg) = val;
}

static inline uint8_t xhci_read8(struct xhci_controller *xhci, uint64_t base, uint64_t reg) {
    (void)xhci;
    return *(volatile uint8_t *)(uintptr_t)(base + reg);
}

/* API */
int  xhci_init(void);
int  xhci_is_present(void);
int  xhci_port_reset(int port);
int  xhci_port_status(int port);
void xhci_print_info(void);

/* ── TRB Ring Management API ──────────────────────────────────────── */
int  xhci_ring_create(struct xhci_ring *ring);
void xhci_ring_destroy(struct xhci_ring *ring);
int  xhci_ring_enqueue(struct xhci_ring *ring, const struct xhci_trb *trb);
int  xhci_ring_dequeue(struct xhci_ring *ring, struct xhci_trb *trb, int consumer_cycle);
int  xhci_ring_has_pending(struct xhci_ring *ring);

int  xhci_event_ring_init(struct xhci_event_ring *ev, int num_trbs);
void xhci_event_ring_fini(struct xhci_event_ring *ev);
int  xhci_event_ring_process(struct xhci_event_ring *ev,
                             void (*handler)(struct xhci_trb *ev_trb, void *ctx),
                             void *ctx);

void xhci_ring_doorbell(struct xhci_controller *xhci, int slot_id, int doorbell_target);

#endif /* XHCI_H */
