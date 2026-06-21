#include "e1000.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "net.h"
#include "netdevice.h"
#include "smp.h"
#ifdef MODULE
#include "module.h"
#endif

/* ── Register offsets ──────────────────────────────────────────────── */
#define REG_CTRL       0x0000
#define REG_STATUS     0x0008
#define REG_EERD       0x0014
#define REG_ICR        0x00C0
#define REG_IMS        0x00D0
#define REG_IMC        0x00D8
#define REG_ITR        0x00C4  /* Interrupt Throttling Rate */
#define REG_RCTL       0x0100
#define REG_TCTL       0x0400
#define REG_RAL        0x5400  /* Receive Address Low */
#define REG_RAH        0x5404  /* Receive Address High */
#define REG_MTA        0x5200  /* Multicast Table Array */

/* Multi-queue / RSS registers (82574 specific) */
#define REG_MRQC       0x5818  /* Multiple Receive Queues Command */
#define REG_RSSRK(n)   (0x5C80 + (n) * 4)  /* RSS Random Key (4 dwords) */
#define REG_RETA(i)    (0x5C00 + (i) * 4)  /* RSS Redirection Table (32 dwords, 4 entries per dword) */

/* Per-queue RX register base offsets (queue 0, 1, 2, 3) */
/* Registers: RDBAL, RDBAH, RDLEN, RDH, RDT */
static const uint32_t RX_Q_REGS[E1000_MAX_QUEUES][5] = {
    {0x2800, 0x2804, 0x2808, 0x2810, 0x2818},  /* queue 0 */
    {0x2C00, 0x2C04, 0x2C08, 0x2C10, 0x2C18},  /* queue 1 */
    {0x3000, 0x3004, 0x3008, 0x3010, 0x3018},  /* queue 2 (82576) */
    {0x3400, 0x3404, 0x3408, 0x3410, 0x3418},  /* queue 3 (82576) */
};

/* Per-queue TX register base offsets */
static const uint32_t TX_Q_REGS[E1000_MAX_QUEUES][5] = {
    {0x3800, 0x3804, 0x3808, 0x3810, 0x3818},  /* queue 0 */
    {0x3C00, 0x3C04, 0x3C08, 0x3C10, 0x3C18},  /* queue 1 */
    {0x4000, 0x4004, 0x4008, 0x4010, 0x4018},  /* queue 2 (82576) */
    {0x4400, 0x4404, 0x4408, 0x4410, 0x4418},  /* queue 3 (82576) */
};

/* ITR rates (value written is interval in 256 ns units).
 *
 * A higher ITR value = longer interval = fewer interrupts/sec.
 * Common rates:
 *   0    – no throttling (interrupt per packet, maximum CPU overhead)
 *   122  – ~31 us interval  (~32,000 int/s, high throughput)
 *   488  – ~125 us interval (~8,000 int/s, balanced)
 *   1953 – ~500 us interval (~2,000 int/s, low CPU)
 *   8000 – ~2 ms interval   (~500 int/s, very low CPU)
 */
/* MSI-X related registers (82576 specific) */
#define REG_MXUTA      0x0580C  /* MSI-X Unmask Table Array */
#define REG_PBACLR     0x01668  /* PBA Clear */
#define REG_IVAR       0x01700  /* Interrupt Vector Allocation Register */
#define REG_IVAR0      REG_IVAR        /* Queue 0 -> vector mapping */
#define REG_IVAR1      (REG_IVAR + 4)  /* Queue 1 -> vector mapping */
#define REG_IVAR2      (REG_IVAR + 8)  /* Queue 2 -> vector mapping */
#define REG_IVAR3      (REG_IVAR + 12) /* Queue 3 -> vector mapping */

/* IVAR entry format (per queue): bits[7:0] = vector, bit[15] = valid */
#define IVAR_ENTRY(vec)  ((vec) | 0x80)  /* lower byte: vector | valid bit */

#define ITR_OFF        0
#define ITR_HIGH       122     /* high throughput (~32K int/s) */
#define ITR_BALANCED   488     /* balanced (~8K int/s) */
#define ITR_LOW        1953    /* low CPU (~2K int/s) */
#define ITR_MINIMAL    8000    /* minimal interrupts (~500 int/s) */

/* Adaptive ITR tunables */
#define ITR_SAMPLE_MS  100     /* re-evaluate every 100 ms */
#define ITR_LOW_PPS    500     /* below this PPS -> less throttling */
#define ITR_HIGH_PPS   5000    /* above this PPS -> more throttling */

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
#define RCTL_MQ     (1 << 24)   /* Multiple Queues (required for RSS) */

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

/* Descriptors — must be 16-byte aligned */
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

/* ── Per-queue state ───────────────────────────────────────────────── */
struct e1000_queue_stats {
    uint64_t rx_packets;       /* total packets received */
    uint64_t tx_packets;       /* total packets transmitted */
    uint64_t rx_bytes;         /* total bytes received */
    uint64_t tx_bytes;         /* total bytes transmitted */
    uint64_t rx_errors;        /* receive errors */
    uint64_t tx_errors;        /* transmit errors */
    uint64_t rx_dropped;       /* packets dropped */
    uint64_t tx_busy;          /* transmit busy (ring full) count */
};

struct e1000_queue {
    struct e1000_rx_desc rx_descs[NUM_RX_DESC] __attribute__((aligned(16)));
    struct e1000_tx_desc tx_descs[NUM_TX_DESC] __attribute__((aligned(16)));
    uint8_t rx_buffers[NUM_RX_DESC][RX_BUF_SIZE] __attribute__((aligned(16)));
    uint8_t tx_buffers[NUM_TX_DESC][RX_BUF_SIZE] __attribute__((aligned(16)));
    int rx_cur;
    int tx_cur;
    struct e1000_queue_stats stats;   /* per-queue statistics */
    int irq_vector;                    /* MSI-X vector (-1 if legacy) */
};

/* ── Global state ──────────────────────────────────────────────────── */
static volatile uint8_t *mmio_base;
static uint8_t mac_addr[6];
static int nic_present = 0;
static uint8_t e1000_irq_line = 0;

/* Per-queue lock for SMP safety (T5) */
static spinlock_t e1000_lock = SPINLOCK_INIT;

/* Device variant */
static int is_82574 = 0;  /* set to 1 if we detect 82574L */
static int is_82576 = 0;  /* set to 1 if we detect 82576 (supports 4 queues) */

/* Number of active queues (1 for 82540EM, 2 for 82574) */
static int num_queues = 1;

/* Per-queue state */
static struct e1000_queue queues[E1000_MAX_QUEUES];

/* RSS hash key (default Intel recommended key) */
static uint32_t rss_key[4] = {
    0x6D5A56DA, 0xBF914C5B, 0xF4A3FCF1, 0x3BB06F25
};

/* ITR adaptive state (per queue — queue 0 ITR used as global) */
static uint32_t itr_current = ITR_BALANCED;
static uint32_t itr_pkt_count = 0;
static uint64_t itr_last_tick = 0;
static int      itr_enabled = 0;

/* ── MMIO helpers ──────────────────────────────────────────────────── */
static void e1000_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(mmio_base + reg) = val;
}

static uint32_t e1000_read(uint32_t reg) {
    return *(volatile uint32_t *)(mmio_base + reg);
}

/* ── Per-queue register helpers ────────────────────────────────────── */
static void e1000_q_write_rx(int q, int reg_idx, uint32_t val) {
    e1000_write(RX_Q_REGS[q][reg_idx], val);
}

static uint32_t e1000_q_read_rx(int q, int reg_idx) {
    return e1000_read(RX_Q_REGS[q][reg_idx]);
}

static void e1000_q_write_tx(int q, int reg_idx, uint32_t val) {
    e1000_write(TX_Q_REGS[q][reg_idx], val);
}

/* ── RSS helpers ───────────────────────────────────────────────────── */

/* Write the RSS random key (4 dwords) to the hardware */
static void e1000_rss_write_key(void) {
    if (!is_82574) return;
    for (int i = 0; i < 4; i++)
        e1000_write(REG_RSSRK(i), rss_key[i]);
}

/* Write the RSS Redirection Table.
 * RETA has 32 dwords, each encoding 4 entries (bytes).
 * Entry i maps to queue (i % num_queues) for a simple round-robin
 * distribution across available queues. */
static void e1000_rss_write_reta(void) {
    if (!is_82574) return;
    uint32_t reta[32];

    for (int i = 0; i < 128; i++) {
        int word = i / 4;
        int shift = (i % 4) * 8;
        uint8_t q = (uint8_t)(i % (uint8_t)num_queues);
        reta[word] &= ~(0xFFu << shift);
        reta[word] |= ((uint32_t)q << shift);
    }

    for (int i = 0; i < 32; i++)
        e1000_write(REG_RETA(i), reta[i]);
}

/* Configure RSS on hardware that supports it (82574).
 * Enables RSS with TCP/IPv4 and IPv4 hashing. */
static void e1000_rss_init(void) {
    if (!is_82574) return;

    /* Write RSS key */
    e1000_rss_write_key();

    /* Write RETA (redirection table) */
    e1000_rss_write_reta();

    /* MRQC: enable RSS with TCP/IPv4 and IPv4 hash types.
     * MRQC bits [2:0] = 001 (RSS enabled) */
    uint32_t mrqc = 1;  /* bit 0: RSS enable */
    mrqc |= E1000_RSS_HASH_TCP_IPV4 | E1000_RSS_HASH_IPV4;  /* hash types */
    e1000_write(REG_MRQC, mrqc);

    kprintf("  e1000: RSS enabled (%d queues, hash TCPv4|IPv4)\n", num_queues);
}

/* ── Interrupt handler ─────────────────────────────────────────────── */

/* IRQ handler — called for interrupt on any queue.
 * On 82574, we could use separate MSI vectors per queue, but for
 * simplicity we use a single legacy IRQ and check all queues. */
static void e1000_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint32_t icr = e1000_read(REG_ICR); /* read to clear */
    (void)icr;

    uint64_t __e1k_flags;
    spinlock_irqsave_acquire(&e1000_lock, &__e1k_flags);

    /* Mask RX interrupt — NAPI-style: re-enable after draining in net_poll */
    e1000_write(REG_IMC, 0x80); /* mask RXT0 */

    /* Count received packets for adaptive ITR */
    if (itr_enabled) {
        uint32_t total_avail = 0;
        for (int q = 0; q < num_queues; q++) {
            uint32_t rdh = e1000_q_read_rx(q, 3);  /* RDH */
            uint32_t rdt = e1000_q_read_rx(q, 4);  /* RDT */
            uint32_t avail;
            if (rdt >= rdh)
                avail = rdt - rdh;
            else
                avail = (NUM_RX_DESC - rdh) + rdt;
            if (avail > 32) avail = 32;
            total_avail += avail;
        }
        itr_pkt_count += (total_avail > 0) ? total_avail : 1;
    }

    spinlock_irqsave_release(&e1000_lock, __e1k_flags);

    irq_ack(e1000_irq_line);
    net_rx_signal();
}

/* ── ITR (Interrupt Throttling) — single instance (queue 0) ────────── */
static void e1000_set_itr(uint32_t value) {
    if (!nic_present) return;
    if (value > 0xFFFF) value = 0xFFFF;
    e1000_write(REG_ITR, value);
    itr_current = value;
}

static void e1000_itr_adaptive(void) {
    if (!itr_enabled || !nic_present) return;

    uint32_t now_lo, now_hi;
    __asm__ volatile("rdtsc" : "=a"(now_lo), "=d"(now_hi));
    uint64_t now = ((uint64_t)now_hi << 32) | now_lo;

    if (itr_last_tick == 0) {
        itr_last_tick = now;
        itr_pkt_count = 0;
        return;
    }

    uint64_t tsc_elapsed = now - itr_last_tick;
    if (now < itr_last_tick) {
        itr_last_tick = now;
        itr_pkt_count = 0;
        return;
    }

    /* Minimum sample time ~50ms */
    if (tsc_elapsed < 100000000ULL)
        return;

    uint32_t approx_pps;
    if (tsc_elapsed > 0) {
        uint64_t scaled = (uint64_t)itr_pkt_count * 2000000000ULL;
        approx_pps = (uint32_t)(scaled / tsc_elapsed);
    } else {
        approx_pps = 0;
    }

    uint32_t new_itr = itr_current;
    if (approx_pps > ITR_HIGH_PPS) {
        new_itr = itr_current + (itr_current / 4);
        if (new_itr > ITR_MINIMAL)
            new_itr = ITR_MINIMAL;
    } else if (approx_pps < ITR_LOW_PPS) {
        if (new_itr > ITR_HIGH) {
            new_itr = itr_current - (itr_current / 4);
            if (new_itr < ITR_HIGH)
                new_itr = ITR_HIGH;
        }
    }

    if (new_itr != itr_current)
        e1000_set_itr(new_itr);

    itr_last_tick = now;
    itr_pkt_count = 0;
}

static void e1000_read_mac(void) {
    uint32_t ral = e1000_read(REG_RAL);
    uint32_t rah = e1000_read(REG_RAH);
    mac_addr[0] = (uint8_t)(ral & 0xFF);
    mac_addr[1] = (uint8_t)((ral >> 8) & 0xFF);
    mac_addr[2] = (uint8_t)((ral >> 16) & 0xFF);
    mac_addr[3] = (uint8_t)((ral >> 24) & 0xFF);
    mac_addr[4] = (uint8_t)(rah & 0xFF);
    mac_addr[5] = (uint8_t)((rah >> 8) & 0xFF);
}

/* ── Per-queue RX/TX initialization ────────────────────────────────── */

static void e1000_init_rx_queue(int q) {
    struct e1000_queue *qp = &queues[q];

    for (int i = 0; i < NUM_RX_DESC; i++) {
        qp->rx_descs[i].addr = VIRT_TO_PHYS(qp->rx_buffers[i]);
        qp->rx_descs[i].status = 0;
    }
    uint64_t rdesc_phys = VIRT_TO_PHYS(qp->rx_descs);
    e1000_q_write_rx(q, 0, (uint32_t)(rdesc_phys & 0xFFFFFFFF));       /* RDBAL */
    e1000_q_write_rx(q, 1, (uint32_t)(rdesc_phys >> 32));              /* RDBAH */
    e1000_q_write_rx(q, 2, NUM_RX_DESC * sizeof(struct e1000_rx_desc));/* RDLEN */
    e1000_q_write_rx(q, 3, 0);                                          /* RDH */
    e1000_q_write_rx(q, 4, NUM_RX_DESC - 1);                           /* RDT */
    qp->rx_cur = 0;
}

static void e1000_init_tx_queue(int q) {
    struct e1000_queue *qp = &queues[q];

    for (int i = 0; i < NUM_TX_DESC; i++) {
        qp->tx_descs[i].addr = VIRT_TO_PHYS(qp->tx_buffers[i]);
        qp->tx_descs[i].status = TDESC_STA_DD;
        qp->tx_descs[i].cmd = 0;
    }
    uint64_t tdesc_phys = VIRT_TO_PHYS(qp->tx_descs);
    e1000_q_write_tx(q, 0, (uint32_t)(tdesc_phys & 0xFFFFFFFF));       /* TDBAL */
    e1000_q_write_tx(q, 1, (uint32_t)(tdesc_phys >> 32));              /* TDBAH */
    e1000_q_write_tx(q, 2, NUM_TX_DESC * sizeof(struct e1000_tx_desc));/* TDLEN */
    e1000_q_write_tx(q, 3, 0);                                          /* TDH */
    e1000_q_write_tx(q, 4, 0);                                          /* TDT */
    qp->tx_cur = 0;
}

/* ── Netdevice callbacks (forward declarations) ─────────────────── */
static int e1000_netdev_transmit(struct net_device *dev,
                                  const uint8_t *data, uint16_t len);
static int e1000_netdev_receive(struct net_device *dev,
                                uint8_t *buf, uint16_t max_len);

/* ── Initialisation ────────────────────────────────────────────────── */

int e1000_init(void) {
    struct pci_device dev;

    /* Try 82540EM first, then 82574, then 82576 */
    if (pci_find_device(E1000_VENDOR, E1000_DEVICE, &dev) >= 0) {
        is_82574 = 0;
        is_82576 = 0;
        num_queues = 1;
    } else if (pci_find_device(E1000_VENDOR, E1000_DEV_82574, &dev) >= 0) {
        is_82574 = 1;
        is_82576 = 0;
        num_queues = E1000_MAX_QUEUES < 2 ? E1000_MAX_QUEUES : 2;
    } else if (pci_find_device(E1000_VENDOR, E1000_DEV_82576, &dev) >= 0) {
        is_82574 = 0;
        is_82576 = 1;
        num_queues = E1000_MAX_QUEUES;  /* up to 4 */
    } else {
        return -1;
    }

    /* Get MMIO base from BAR0 (memory-mapped, mask lower 4 bits) */
    uint64_t bar0 = dev.bar[0] & ~0xFULL;
    kprintf("  e1000: %s BAR0=0x%llx IRQ=%lu queues=%d\n",
            is_82576 ? "82576" : (is_82574 ? "82574L" : "82540EM"),
            (unsigned long long)bar0, (unsigned long)dev.irq, num_queues);

    /* Map MMIO region (128KB) into high-half VMA space */
    mmio_base = (volatile uint8_t *)vmm_map_phys(bar0, 0x20000,
                    VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    if (!mmio_base) {
        kprintf("  e1000: failed to map MMIO\n");
        return -1;
    }

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

    /* Enable RX interrupt on queue 0; clear pending */
    e1000_write(REG_IMC, 0xFFFFFFFF);
    e1000_read(REG_ICR);
    e1000_write(REG_IMS, 0x80); /* RXT0 */
    e1000_irq_line = dev.irq;
    idt_register_handler(32 + dev.irq, e1000_irq_handler);
    if (apic_is_init_complete())
        ioapic_unmask_irq(dev.irq);
    pic_unmask(dev.irq);

    /* Initialize per-queue state */
    for (int q = 0; q < E1000_MAX_QUEUES; q++) {
        queues[q].irq_vector = -1;
        memset(&queues[q].stats, 0, sizeof(queues[q].stats));
    }

    /* Configure per-queue interrupt vectors via IVAR (82574/82576).
     * Each queue gets a separate MSI-X-style vector if MSI-X is available,
     * otherwise they share the legacy IRQ. */
    if (is_82574 || is_82576) {
        /* For now, use a single legacy IRQ for all queues.
         * On real hardware with MSI-X, each queue would get its own vector. */
        for (int q = 0; q < num_queues; q++) {
            uint32_t ivar_reg;
            uint32_t ivar_val;
            int reg_idx = q / 2;  /* two entries per IVAR register */
            int byte_shift = (q % 2) * 8;
            /* Each queue entry in IVAR: bits[7:0] = vector, bit[7] = valid */
            ivar_val = IVAR_ENTRY(e1000_irq_line);
            ivar_reg = e1000_read(REG_IVAR + reg_idx * 4);
            ivar_reg &= ~(0xFFu << byte_shift);
            ivar_reg |= (ivar_val << byte_shift);
            e1000_write(REG_IVAR + reg_idx * 4, ivar_reg);
        }
    }

    /* Clear multicast table */
    for (int i = 0; i < 128; i++)
        e1000_write(REG_MTA + i * 4, 0);

    e1000_read_mac();

    /* Initialize RX queues */
    for (int q = 0; q < num_queues; q++)
        e1000_init_rx_queue(q);

    /* Initialize TX queues */
    for (int q = 0; q < num_queues; q++)
        e1000_init_tx_queue(q);

    /* Enable RX and TX with RCTL */
    uint32_t rctl = RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC;
    if (is_82574)
        rctl |= RCTL_MQ;  /* enable multiple queues */
    e1000_write(REG_RCTL, rctl);

    e1000_write(REG_TCTL, TCTL_EN | TCTL_PSP | (15 << 4) | (64 << 12));

    nic_present = 1;

    /* Configure RSS if hardware supports it */
    e1000_rss_init();

    /* Start ITR */
    {
        e1000_set_itr(ITR_BALANCED);
        itr_pkt_count = 0;
        itr_last_tick = 0;
        uint32_t init_lo, init_hi;
        __asm__ volatile("rdtsc" : "=a"(init_lo), "=d"(init_hi));
        itr_last_tick = ((uint64_t)init_hi << 32) | init_lo;
        itr_enabled = 1;
        kprintf("  e1000: ITR enabled (rate=%u, ~%u int/s)\n",
                (unsigned)ITR_BALANCED,
                (unsigned)(1000000000ULL / (ITR_BALANCED * 256ULL + 1)));
    }

    /* ── Register with netdevice layer ──────────────────────────── */
    {
        static struct net_device ndev;
        int reg_ok = 0;

        if (netif_name_to_index("eth0") < 0) {
            memset(&ndev, 0, sizeof(ndev));
            snprintf(ndev.name, sizeof(ndev.name), "eth0");
            memcpy(ndev.mac, mac_addr, 6);
            ndev.transmit = e1000_netdev_transmit;
            ndev.receive  = e1000_netdev_receive;
            ndev.mtu      = 1500;
            ndev.flags    = 1; /* IFF_UP */
            ndev.priv     = NULL;

            int ifindex = netif_register(&ndev);
            if (ifindex >= 0) {
                kprintf("[e1000] registered as netdevice ifindex=%d\n", ifindex);
                reg_ok = 1;
            }
        } else {
            reg_ok = 1;
        }
        (void)reg_ok;
    }

    kprintf("[OK] e1000: %s up (MAC %02x:%02x:%02x:%02x:%02x:%02x, %d RX/TX queues)\n",
            is_82576 ? "82576" : (is_82574 ? "82574L" : "82540EM"),
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5],
            num_queues);

    return 0;
}

int e1000_is_present(void) {
    return nic_present;
}

void e1000_get_mac(uint8_t *mac) {
    memcpy(mac, mac_addr, 6);
}

/* Re-enable RX interrupts after NAPI-style polling drain */
void e1000_irq_rearm(void) {
    if (nic_present) {
        uint64_t __e1k_flags;
        spinlock_irqsave_acquire(&e1000_lock, &__e1k_flags);
        e1000_itr_adaptive();
        spinlock_irqsave_release(&e1000_lock, __e1k_flags);
        e1000_write(REG_IMS, 0x80); /* unmask RXT0 */
    }
}

/* Return the number of RX queues active */
int e1000_rx_queue_count(void) {
    return num_queues;
}

/* ── Netdevice callbacks ───────────────────────────────────────────── */

/* Transmit — queue selection via hash of packet data to distribute
 * across available TX queues.  For single-queue, always queue 0.
 * For multi-queue, a simple hash based on first bytes distributes load. */
static int e1000_netdev_transmit(struct net_device *dev,
                                  const uint8_t *data, uint16_t len)
{
    (void)dev;

    kprintf("[dbg] e1000_netdev_transmit: len=%u\n", len);

    /* Choose a TX queue: hash src+dst IP from ethernet/IP header if possible */
    int tx_q = 0;
    if (num_queues > 1 && len > 14) {
        /* Simple hash: XOR of first 4 bytes of payload (typically IP dst) */
        uint32_t hash = 0;
        for (int i = 0; i < 4 && (14 + i) < len; i++)
            hash = (hash << 8) | data[14 + i];
        tx_q = (int)(hash % (uint32_t)num_queues);
    }

    struct e1000_queue *qp = &queues[tx_q];
    int idx = qp->tx_cur;

    /* Quick check: if the next descriptor in the ring is still in-flight,
     * the TX queue is full. Return BUSY so the upper layer can retry
     * instead of busy-waiting or silently dropping. */
    int next_idx = (idx + 1) % NUM_TX_DESC;
    if (!(qp->tx_descs[next_idx].status & TDESC_STA_DD)) {
        /* Ring full — upper layer should try again later */
        qp->stats.tx_busy++;
        return NETDEV_TX_BUSY;
    }

    /* Wait for current descriptor to be available */
    int tx_timeout = 10000000;
    while (!(qp->tx_descs[idx].status & TDESC_STA_DD) && --tx_timeout > 0)
        __asm__ volatile("pause");
    if (tx_timeout <= 0) {
        kprintf("[dbg] e1000_netdev_transmit: TIMEOUT waiting for desc %d\n", idx);
        qp->stats.tx_errors++;
        return -1;
    }

    uint64_t __e1k_flags;
    spinlock_irqsave_acquire(&e1000_lock, &__e1k_flags);

    memcpy(qp->tx_buffers[idx], data, len);
    qp->tx_descs[idx].length = len;
    qp->tx_descs[idx].cmd = TDESC_CMD_EOP | TDESC_CMD_IFCS | TDESC_CMD_RS;
    qp->tx_descs[idx].status = 0;

    qp->tx_cur = (qp->tx_cur + 1) % NUM_TX_DESC;
    e1000_q_write_tx(tx_q, 4, qp->tx_cur); /* TDT */

    spinlock_irqsave_release(&e1000_lock, __e1k_flags);

    /* Update per-queue transmit stats */
    qp->stats.tx_packets++;
    qp->stats.tx_bytes += len;

    kprintf("[dbg] e1000_netdev_transmit: done, new tx_cur=%u\n", qp->tx_cur);
    return 0;
}

/* Receive — poll all queues for received packets.
 * On multi-queue devices, this checks each queue in turn and returns
 * the first available packet. */
static int e1000_netdev_receive(struct net_device *dev,
                                uint8_t *buf, uint16_t max_len)
{
    (void)dev;

    uint64_t __e1k_flags;
    spinlock_irqsave_acquire(&e1000_lock, &__e1k_flags);

    for (int q = 0; q < num_queues; q++) {
        struct e1000_queue *qp = &queues[q];
        int idx = qp->rx_cur;

        if (!(qp->rx_descs[idx].status & RDESC_STA_DD))
            continue; /* nothing on this queue */

        /* Check for errors */
        if (qp->rx_descs[idx].errors != 0) {
            qp->rx_descs[idx].status = 0;
            int old_cur = qp->rx_cur;
            qp->rx_cur = (qp->rx_cur + 1) % NUM_RX_DESC;
            e1000_q_write_rx(q, 4, old_cur); /* RDT */
            qp->stats.rx_errors++;
            continue;
        }

        /* Check End-Of-Packet */
        if (!(qp->rx_descs[idx].status & RDESC_STA_EOP)) {
            qp->rx_descs[idx].status = 0;
            int old_cur = qp->rx_cur;
            qp->rx_cur = (qp->rx_cur + 1) % NUM_RX_DESC;
            e1000_q_write_rx(q, 4, old_cur);
            qp->stats.rx_dropped++;
            continue;
        }

        uint16_t len = qp->rx_descs[idx].length;
        if (len > max_len) len = max_len;
        memcpy(buf, qp->rx_buffers[idx], len);

        qp->rx_descs[idx].status = 0;
        int old_cur = qp->rx_cur;
        qp->rx_cur = (qp->rx_cur + 1) % NUM_RX_DESC;
        e1000_q_write_rx(q, 4, old_cur);

        /* Update per-queue receive stats */
        qp->stats.rx_packets++;
        qp->stats.rx_bytes += len;

        spinlock_irqsave_release(&e1000_lock, __e1k_flags);
        return (int)len;
    }

    spinlock_irqsave_release(&e1000_lock, __e1k_flags);
    return 0; /* nothing on any queue */
}

/* ── Legacy API (used by older shell/net paths) ─────────────────────── */

int e1000_send(const void *data, uint16_t len) {
    /* Route to queue 0 for legacy callers */
    if (!nic_present || len > RX_BUF_SIZE) return -1;

    struct e1000_queue *qp = &queues[0];
    int idx = qp->tx_cur;

    int tx_timeout = 10000000;
    while (!(qp->tx_descs[idx].status & TDESC_STA_DD) && --tx_timeout > 0)
        __asm__ volatile("pause");
    if (tx_timeout <= 0) return -1;

    uint64_t __e1k_flags;
    spinlock_irqsave_acquire(&e1000_lock, &__e1k_flags);

    memcpy(qp->tx_buffers[idx], data, len);
    qp->tx_descs[idx].length = len;
    qp->tx_descs[idx].cmd = TDESC_CMD_EOP | TDESC_CMD_IFCS | TDESC_CMD_RS;
    qp->tx_descs[idx].status = 0;

    qp->tx_cur = (qp->tx_cur + 1) % NUM_TX_DESC;
    e1000_q_write_tx(0, 4, qp->tx_cur);

    spinlock_irqsave_release(&e1000_lock, __e1k_flags);

    return 0;
}

int e1000_receive(void *buf, uint16_t max_len) {
    uint64_t __e1k_flags;
    spinlock_irqsave_acquire(&e1000_lock, &__e1k_flags);

    /* Check all queues, starting with queue 0 */
    for (int q = 0; q < num_queues; q++) {
        struct e1000_queue *qp = &queues[q];
        int idx = qp->rx_cur;

        if (!(qp->rx_descs[idx].status & RDESC_STA_DD))
            continue;

        if (qp->rx_descs[idx].errors != 0) {
            qp->rx_descs[idx].status = 0;
            int old_cur = qp->rx_cur;
            qp->rx_cur = (qp->rx_cur + 1) % NUM_RX_DESC;
            e1000_q_write_rx(q, 4, old_cur);
            spinlock_irqsave_release(&e1000_lock, __e1k_flags);
            return -2;
        }

        if (!(qp->rx_descs[idx].status & RDESC_STA_EOP)) {
            qp->rx_descs[idx].status = 0;
            int old_cur = qp->rx_cur;
            qp->rx_cur = (qp->rx_cur + 1) % NUM_RX_DESC;
            e1000_q_write_rx(q, 4, old_cur);
            spinlock_irqsave_release(&e1000_lock, __e1k_flags);
            return -3;
        }

        uint16_t len = qp->rx_descs[idx].length;
        if (len > max_len) len = max_len;
        memcpy(buf, qp->rx_buffers[idx], len);

        qp->rx_descs[idx].status = 0;
        int old_cur = qp->rx_cur;
        qp->rx_cur = (qp->rx_cur + 1) % NUM_RX_DESC;
        e1000_q_write_rx(q, 4, old_cur);

        qp->stats.rx_packets++;
        qp->stats.rx_bytes += len;

        spinlock_irqsave_release(&e1000_lock, __e1k_flags);
        return len;
    }

    spinlock_irqsave_release(&e1000_lock, __e1k_flags);
    return 0;
}

/* ── Module lifecycle ──────────────────────────────────────────────── */

void e1000_exit(void) {
    if (!nic_present) return;

    kprintf("[e1000] Shutting down NIC...\n");

    /* Disable all interrupts */
    e1000_write(REG_IMC, 0xFFFFFFFF);
    e1000_read(REG_ICR);

    /* Mask IRQ */
    if (apic_is_init_complete())
        ioapic_mask_irq(e1000_irq_line);
    pic_mask(e1000_irq_line);

    /* Disable RX and TX */
    e1000_write(REG_RCTL, 0);
    e1000_write(REG_TCTL, 0);

    /* Reset */
    e1000_write(REG_CTRL, e1000_read(REG_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 100000; i++);

    /* Unmap MMIO */
    if (mmio_base) {
        vmm_unmap_phys((void *)mmio_base, 0x20000);
        mmio_base = NULL;
    }

    nic_present = 0;

    /* Unregister netdevice */
    {
        int idx = netif_name_to_index("eth0");
        if (idx >= 0)
            netif_unregister(idx);
    }

    kprintf("[e1000] NIC shut down\n");
}

#ifdef MODULE
int init_module(void) {
    return e1000_init();
}
void cleanup_module(void) {
    e1000_exit();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Intel PRO/1000 (E1000/E1000E) PCI Ethernet driver with multi-queue RSS");
MODULE_ALIAS("pci:v00008086d0000100Esv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d000010D3sv*sd*bc*sc*i*");
#endif /* MODULE */

/* ── e1000_open: Enable RX/TX, set up interrupts ──────────── */
int e1000_open(void *dev)
{
    (void)dev;
    if (!nic_present) return -EIO;

    kprintf("[e1000] Opening NIC...\n");

    /* Enable receiver */
    uint32_t rctl = e1000_read(REG_RCTL);
    rctl |= RCTL_EN | RCTL_BAM | RCTL_SECRC;
    rctl &= ~(RCTL_UPE | RCTL_MPE); /* no promisc */
    e1000_write(REG_RCTL, rctl);

    /* Enable transmitter */
    uint32_t tctl = e1000_read(REG_TCTL);
    tctl |= TCTL_EN | TCTL_PSP;
    e1000_write(REG_TCTL, tctl);

    /* Enable interrupts: TXQE, RXQ0, LINK */
    e1000_write(REG_IMS, (1u << 6) | (1u << 7) | (1u << 2));
    e1000_read(REG_ICR); /* clear pending */

    kprintf("[e1000] NIC opened\n");
    return 0;
}

/* ── e1000_stop: Disable RX/TX, mask interrupts ──────────── */
int e1000_stop(void *dev)
{
    (void)dev;
    if (!nic_present) return -EIO;

    kprintf("[e1000] Stopping NIC...\n");

    /* Mask all interrupts */
    e1000_write(REG_IMC, 0xFFFFFFFF);

    /* Disable TX and RX */
    e1000_write(REG_TCTL, 0);
    e1000_write(REG_RCTL, 0);

    kprintf("[e1000] NIC stopped\n");
    return 0;
}

/* ── e1000_set_multicast: Update multicast filters ──────────── */
int e1000_set_multicast(void *dev, void *addr, int count)
{
    (void)dev;
    if (!nic_present) return -EIO;

    if (count == 0) {
        /* Disable multicast promiscuous, clear MTA */
        uint32_t rctl = e1000_read(REG_RCTL);
        rctl &= ~RCTL_MPE;
        e1000_write(REG_RCTL, rctl);
        for (int i = 0; i < 128; i++)
            e1000_write(REG_MTA + i * 4, 0);
        return 0;
    }

    /* Enable multicast promiscuous for simplicity */
    uint32_t rctl = e1000_read(REG_RCTL);
    rctl |= RCTL_MPE;
    e1000_write(REG_RCTL, rctl);

    /* If we had a hash table, we'd program MTA entries here */
    (void)addr;
    (void)count;

    kprintf("[e1000] Multicast filter set: %d address(es)\n", count);
    return 0;
}
