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
#define XHCI_CMD_RUN        (1 << 0)
#define XHCI_CMD_HCRST      (1 << 1)
#define XHCI_CMD_INTE       (1 << 2)
#define XHCI_CMD_HSEE       (1 << 3)

/* USBSTS bits */
#define XHCI_STS_HCH        (1 << 0)
#define XHCI_STS_HSE        (1 << 2)
#define XHCI_STS_EINT       (1 << 3)
#define XHCI_STS_PCD        (1 << 4)
#define XHCI_STS_CNR        (1 << 11)

/* Port register set */
#define XHCI_PORTSC         0x400  /* Port Status and Control (per port, 0x10 each) */
#define XHCI_PORTPMSC       0x404  /* Port Power Management */
#define XHCI_PORTLI         0x408  /* Port Link Info */
#define XHCI_PORTHLPMC      0x40C  /* Port Hardware LPM Control */

/* PORTSC bits */
#define XHCI_PORTSC_CCS     (1 << 0)
#define XHCI_PORTSC_PED     (1 << 1)
#define XHCI_PORTSC_PR      (1 << 4)
#define XHCI_PORTSC_PP      (1 << 9)
#define XHCI_PORTSC_CSC     (1 << 16)
#define XHCI_PORTSC_PEC     (1 << 17)
#define XHCI_PORTSC_WRC     (1 << 19)
#define XHCI_PORTSC_OCC     (1 << 20)
#define XHCI_PORTSC_PRC     (1 << 21)
#define XHCI_PORTSC_PLC     (1 << 22)
#define XHCI_PORTSC_CEC     (1 << 23)
#define XHCI_PORTSC_WCE     (1 << 24)
#define XHCI_PORTSC_WDE     (1 << 25)

/* Port speed */
#define XHCI_PORTSC_PS_SHIFT 10
#define XHCI_PORTSC_PS_MASK  0x0F

/* Max ports */
#define XHCI_MAX_PORTS 16

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

#endif /* XHCI_H */
