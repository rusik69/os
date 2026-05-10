#include "e1000.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"

/* E1000 register offsets */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_RAL     0x5400
#define REG_RAH     0x5404
#define REG_MTA     0x5200

/* CTRL bits */
#define CTRL_SLU    (1 << 6)    /* Set Link Up */
#define CTRL_RST    (1 << 26)

/* RCTL bits */
#define RCTL_EN     (1 << 1)
#define RCTL_UPE    (1 << 3)    /* Unicast promisc */
#define RCTL_MPE    (1 << 4)    /* Multicast promisc */
#define RCTL_BAM    (1 << 15)   /* Broadcast accept */
#define RCTL_BSIZE_2048 0       /* buffer size 2048 */
#define RCTL_SECRC  (1 << 26)   /* Strip CRC */

/* TCTL bits */
#define TCTL_EN     (1 << 1)
#define TCTL_PSP    (1 << 3)

/* TX descriptor command bits */
#define TDESC_CMD_EOP  (1 << 0)
#define TDESC_CMD_IFCS (1 << 1)
#define TDESC_CMD_RS   (1 << 3)

/* TX/RX descriptor status */
#define TDESC_STA_DD   (1 << 0)
#define RDESC_STA_DD   (1 << 0)
#define RDESC_STA_EOP  (1 << 1)

#define NUM_RX_DESC 32
#define NUM_TX_DESC 32
#define RX_BUF_SIZE 2048

/* Descriptors - must be 16-byte aligned */
struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

/* Static buffers (identity-mapped, so phys == virt for DMA) */
static struct e1000_rx_desc rx_descs[NUM_RX_DESC] __attribute__((aligned(16)));
static struct e1000_tx_desc tx_descs[NUM_TX_DESC] __attribute__((aligned(16)));
static uint8_t rx_buffers[NUM_RX_DESC][RX_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[NUM_TX_DESC][RX_BUF_SIZE] __attribute__((aligned(16)));

static volatile uint8_t *mmio_base;
static uint8_t mac_addr[6];
static int nic_present = 0;
static int rx_cur = 0;
static int tx_cur = 0;

static void e1000_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(mmio_base + reg) = val;
}

static uint32_t e1000_read(uint32_t reg) {
    return *(volatile uint32_t *)(mmio_base + reg);
}

static void e1000_read_mac(void) {
    uint32_t ral = e1000_read(REG_RAL);
    uint32_t rah = e1000_read(REG_RAH);
    mac_addr[0] = ral & 0xFF;
    mac_addr[1] = (ral >> 8) & 0xFF;
    mac_addr[2] = (ral >> 16) & 0xFF;
    mac_addr[3] = (ral >> 24) & 0xFF;
    mac_addr[4] = rah & 0xFF;
    mac_addr[5] = (rah >> 8) & 0xFF;
}

static void e1000_init_rx(void) {
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_descs[i].addr = VIRT_TO_PHYS(rx_buffers[i]);
        rx_descs[i].status = 0;
    }
    uint64_t rdesc_phys = VIRT_TO_PHYS(rx_descs);
    e1000_write(REG_RDBAL, (uint32_t)(rdesc_phys & 0xFFFFFFFF));
    e1000_write(REG_RDBAH, (uint32_t)(rdesc_phys >> 32));
    e1000_write(REG_RDLEN, NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    e1000_write(REG_RDH, 0);
    e1000_write(REG_RDT, NUM_RX_DESC - 1);
    rx_cur = 0;

    e1000_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);
}

static void e1000_init_tx(void) {
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_descs[i].addr = VIRT_TO_PHYS(tx_buffers[i]);
        tx_descs[i].status = TDESC_STA_DD;
        tx_descs[i].cmd = 0;
    }
    uint64_t tdesc_phys = VIRT_TO_PHYS(tx_descs);
    e1000_write(REG_TDBAL, (uint32_t)(tdesc_phys & 0xFFFFFFFF));
    e1000_write(REG_TDBAH, (uint32_t)(tdesc_phys >> 32));
    e1000_write(REG_TDLEN, NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    e1000_write(REG_TDH, 0);
    e1000_write(REG_TDT, 0);
    tx_cur = 0;

    e1000_write(REG_TCTL, TCTL_EN | TCTL_PSP | (15 << 4) | (64 << 12));
}

int e1000_init(void) {
    struct pci_device dev;
    if (pci_find_device(E1000_VENDOR, E1000_DEVICE, &dev) < 0)
        return -1;

    /* Get MMIO base from BAR0 (memory-mapped, mask lower 4 bits) */
    uint64_t bar0 = dev.bar[0] & ~0xFULL;
    kprintf("  e1000 BAR0=0x%x IRQ=%u\n", bar0, (uint64_t)dev.irq);

    /* Map MMIO region (128KB) into virtual address space */
    for (uint64_t off = 0; off < 0x20000; off += PAGE_SIZE) {
        vmm_map_page(bar0 + off, bar0 + off, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    }
    mmio_base = (volatile uint8_t *)(uintptr_t)bar0;

    /* Enable PCI bus mastering */
    pci_enable_bus_master(&dev);

    /* Reset */
    e1000_write(REG_CTRL, e1000_read(REG_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 100000; i++);
    
    /* Set link up */
    e1000_write(REG_CTRL, e1000_read(REG_CTRL) | CTRL_SLU);

    /* Wait for link up (STATUS bit 1) */
    for (volatile int w = 0; w < 5000000; w++) {
        if (e1000_read(REG_STATUS) & 0x02) break;
    }

    /* Disable interrupts (we poll) */
    e1000_write(REG_IMC, 0xFFFFFFFF);
    e1000_read(REG_ICR);

    /* Clear multicast table */
    for (int i = 0; i < 128; i++)
        e1000_write(REG_MTA + i * 4, 0);

    e1000_read_mac();
    e1000_init_rx();
    e1000_init_tx();

    nic_present = 1;
    return 0;
}

int e1000_is_present(void) {
    return nic_present;
}

void e1000_get_mac(uint8_t *mac) {
    memcpy(mac, mac_addr, 6);
}

int e1000_send(const void *data, uint16_t len) {
    if (!nic_present || len > RX_BUF_SIZE) return -1;

    int idx = tx_cur;
    /* Wait for descriptor to be available */
    while (!(tx_descs[idx].status & TDESC_STA_DD));

    memcpy(tx_buffers[idx], data, len);
    tx_descs[idx].length = len;
    tx_descs[idx].cmd = TDESC_CMD_EOP | TDESC_CMD_IFCS | TDESC_CMD_RS;
    tx_descs[idx].status = 0;

    tx_cur = (tx_cur + 1) % NUM_TX_DESC;
    e1000_write(REG_TDT, tx_cur);

    return 0;
}

int e1000_receive(void *buf, uint16_t max_len) {
    if (!nic_present) return -1;

    int idx = rx_cur;
    if (!(rx_descs[idx].status & RDESC_STA_DD))
        return 0; /* nothing to receive */

    uint16_t len = rx_descs[idx].length;
    if (len > max_len) len = max_len;
    memcpy(buf, rx_buffers[idx], len);

    rx_descs[idx].status = 0;
    int old_cur = rx_cur;
    rx_cur = (rx_cur + 1) % NUM_RX_DESC;
    e1000_write(REG_RDT, old_cur);

    return len;
}
