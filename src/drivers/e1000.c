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
#ifdef MODULE
#include "module.h"
#endif

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
#define REG_ITR     0x00C4  /* Interrupt Throttling Rate */

/* ITR rates (value written is interval in 256 ns units).
 * A higher ITR value = longer interval = fewer interrupts/sec.
 * Common rates:
 *   0    – no throttling (interrupt per packet, maximum CPU overhead)
 *   122  – ~31 μs interval  (~32,000 int/s, high throughput)
 *   488  – ~125 μs interval (~8,000 int/s, balanced)
 *   1953 – ~500 μs interval (~2,000 int/s, low CPU)
 *   8000 – ~2 ms interval   (~500 int/s, very low CPU)
 */
#define ITR_OFF        0
#define ITR_HIGH       122     /* high throughput (~32K int/s) */
#define ITR_BALANCED   488     /* balanced (~8K int/s) */
#define ITR_LOW        1953    /* low CPU (~2K int/s) */
#define ITR_MINIMAL    8000    /* minimal interrupts (~500 int/s) */

/* Adaptive ITR tunables */
#define ITR_SAMPLE_MS  100     /* re-evaluate every 100 ms */
#define ITR_LOW_PPS    500     /* below this PPS → less throttling */
#define ITR_HIGH_PPS   5000    /* above this PPS → more throttling */

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
static uint8_t e1000_irq_line = 0;

/* ── Adaptive interrupt moderation (ITR) state ──────────────────── */
static uint32_t itr_current = ITR_BALANCED;   /* current ITR register value */
static uint32_t itr_pkt_count = 0;             /* packets since last adjustment */
static uint64_t itr_last_tick = 0;             /* timer tick of last adjustment */
static int      itr_enabled = 0;               /* adaptive algorithm active */

static void e1000_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(mmio_base + reg) = val;
}

static uint32_t e1000_read(uint32_t reg) {
    return *(volatile uint32_t *)(mmio_base + reg);
}

static void e1000_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint32_t icr = e1000_read(REG_ICR); /* read to clear, also see what caused it */
    (void)icr;
    /* Mask RX interrupt — NAPI-style: we'll re-enable after draining in net_poll */
    e1000_write(REG_IMC, 0x80); /* mask RXT0 */
    irq_ack(e1000_irq_line);

    /* Count the interrupt for adaptive ITR (multiple packets may be pending) */
    if (itr_enabled) {
        /* The ICR's RXT0 bit (bit 7) indicates a receive interrupt.
         * Count approximately how many packets arrived by sampling RDT. */
        uint32_t rdh = e1000_read(REG_RDH);
        uint32_t rdt = e1000_read(REG_RDT);
        /* Packets available = (rdt - rdh) mod NUM_RX_DESC, bounded */
        uint32_t avail;
        if (rdt >= rdh)
            avail = rdt - rdh;
        else
            avail = (NUM_RX_DESC - rdh) + rdt;
        /* Cap at a reasonable burst */
        if (avail > 32) avail = 32;
        itr_pkt_count += (avail > 0) ? avail : 1;
    }

    net_rx_signal();
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

/* ── Interrupt Throttling (ITR) ─────────────────────────────────── */

/*
 * e1000_set_itr — Set the interrupt throttling rate register.
 *
 * @value: ITR value in 256 ns units (the register field).
 *         0 = no throttling, higher = fewer interrupts.
 */
static void e1000_set_itr(uint32_t value) {
    if (!nic_present) return;
    /* Clamp to a 16-bit register field */
    if (value > 0xFFFF) value = 0xFFFF;
    e1000_write(REG_ITR, value);
    itr_current = value;
}

/*
 * e1000_itr_adaptive — Adjust ITR rate based on observed packet rate.
 *
 * Called periodically from the IRQ handler. Counts packets received
 * during ITR_SAMPLE_MS and adjusts the ITR register accordingly:
 *   - High packet rate  → more throttling (fewer interrupts, higher throughput)
 *   - Low packet rate   → less throttling (lower latency)
 */
static void e1000_itr_adaptive(void) {
    if (!itr_enabled || !nic_present) return;

    /* Read timer ticks (assuming ~100 Hz timer) via RDTSC as a coarse estimate */
    uint32_t now_lo, now_hi;
    __asm__ volatile("rdtsc" : "=a"(now_lo), "=d"(now_hi));
    uint64_t now = ((uint64_t)now_hi << 32) | now_lo;

    /* Need a reference tick to compare — use TSC-direct if no timer ticks variable */
    if (itr_last_tick == 0) {
        itr_last_tick = now;
        itr_pkt_count = 0;
        return;
    }

    /* Estimate elapsed time: if TSC frequency is ~2 GHz, 100 ms = 200M ticks.
     * We approximate by checking a reasonable TSC delta (200M ± 50%).
     * A more precise approach would use timer_get_ticks() if available. */
    uint64_t tsc_elapsed = now - itr_last_tick;

    /* If the TSC wraps around, restart */
    if (now < itr_last_tick) {
        itr_last_tick = now;
        itr_pkt_count = 0;
        return;
    }

    /* Re-evaluate only after roughly ITR_SAMPLE_MS have elapsed.
     * At 2 GHz, ITR_SAMPLE_MS = 100 ms → 200,000,000 TSC ticks.
     * Use a generous comparison to handle variable TSC frequencies. */
    if (tsc_elapsed < 100000000ULL)  /* ~50 ms at 2 GHz — minimum sample time */
        return;

    /* Compute packets per second (scaled to avoid float) */
    /* pps = pkt_count / (tsc_elapsed / tsc_freq_hz) */
    /* We approximate tsc_freq_hz ~ 2 GHz */
    uint32_t approx_pps;
    if (tsc_elapsed > 0) {
        /* pps = pkt_count * (2G / tsc_elapsed). To avoid overflow, scale. */
        uint64_t scaled = (uint64_t)itr_pkt_count * 2000000000ULL;
        approx_pps = (uint32_t)(scaled / tsc_elapsed);
    } else {
        approx_pps = 0;
    }

    uint32_t new_itr = itr_current;

    if (approx_pps > ITR_HIGH_PPS) {
        /* High throughput: increase throttling (reduce interrupts) */
        new_itr = itr_current + (itr_current / 4);  /* +25% */
        if (new_itr > ITR_MINIMAL)
            new_itr = ITR_MINIMAL;
    } else if (approx_pps < ITR_LOW_PPS) {
        /* Low throughput: decrease throttling (improve latency) */
        if (new_itr > ITR_HIGH) {
            new_itr = itr_current - (itr_current / 4);  /* -25% */
            if (new_itr < ITR_HIGH)
                new_itr = ITR_HIGH;
        }
    }
    /* else: moderate throughput — keep current setting */

    if (new_itr != itr_current) {
        e1000_set_itr(new_itr);
    }

    itr_last_tick = now;
    itr_pkt_count = 0;
}

/*
 * e1000_itr_init — Initialise interrupt moderation with a balanced rate.
 */
static void e1000_itr_init(void) {
    /* Start with balanced setting */
    e1000_set_itr(ITR_BALANCED);
    itr_pkt_count = 0;
    itr_last_tick = 0;

    /* Read initial TSC reference */
    uint32_t init_lo, init_hi;
    __asm__ volatile("rdtsc" : "=a"(init_lo), "=d"(init_hi));
    itr_last_tick = ((uint64_t)init_hi << 32) | init_lo;

    itr_enabled = 1;
    kprintf("  e1000: ITR enabled (rate=%u, ~%u int/s)\n",
            (unsigned)ITR_BALANCED,
            (unsigned)(1000000000ULL / (ITR_BALANCED * 256ULL + 1)));
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
    kprintf("  e1000 BAR0=0x%llx IRQ=%lu\n", (unsigned long long)bar0, (unsigned long)dev.irq);

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

    /* Enable RX interrupt; clear pending */
    e1000_write(REG_IMC, 0xFFFFFFFF);
    e1000_read(REG_ICR);
    e1000_write(REG_IMS, 0x80); /* RXT0 */
    e1000_irq_line = dev.irq;
    idt_register_handler(32 + dev.irq, e1000_irq_handler);
    if (apic_is_init_complete())
        ioapic_unmask_irq(dev.irq);
    pic_unmask(dev.irq);

    /* Clear multicast table */
    for (int i = 0; i < 128; i++)
        e1000_write(REG_MTA + i * 4, 0);

    e1000_read_mac();
    e1000_init_rx();
    e1000_init_tx();

    nic_present = 1;
    e1000_itr_init();
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
        /* Adjust ITR based on recent packet rate before re-enabling interrupts */
        e1000_itr_adaptive();
        e1000_write(REG_IMS, 0x80); /* unmask RXT0 */
    }
}

int e1000_send(const void *data, uint16_t len) {
    if (!nic_present || len > RX_BUF_SIZE) return -1;

    int idx = tx_cur;
    /* Wait for descriptor to be available */
    int tx_timeout = 10000000;
    while (!(tx_descs[idx].status & TDESC_STA_DD) && --tx_timeout > 0)
        __asm__ volatile("pause");
    if (tx_timeout <= 0) return -1;

    memcpy(tx_buffers[idx], data, len);
    tx_descs[idx].length = len;
    tx_descs[idx].cmd = TDESC_CMD_EOP | TDESC_CMD_IFCS | TDESC_CMD_RS;
    tx_descs[idx].status = 0;

    tx_cur = (tx_cur + 1) % NUM_TX_DESC;
    e1000_write(REG_TDT, tx_cur);

    return 0;
}

/* ── e1000_exit — reverse the initialization, for module unloading ── */
void e1000_exit(void) {
    if (!nic_present) return;

    kprintf("[e1000] Shutting down NIC...\n");

    /* Disable all interrupts */
    e1000_write(REG_IMC, 0xFFFFFFFF);
    e1000_read(REG_ICR); /* clear pending */

    /* Mask IRQ in I/O APIC and PIC */
    if (apic_is_init_complete())
        ioapic_mask_irq(e1000_irq_line);
    pic_mask(e1000_irq_line);

    /* Disable RX and TX */
    e1000_write(REG_RCTL, 0);
    e1000_write(REG_TCTL, 0);

    /* Reset the controller */
    e1000_write(REG_CTRL, e1000_read(REG_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 100000; i++);

    /* Unmap MMIO region */
    if (mmio_base) {
        vmm_unmap_phys((void *)mmio_base, 0x20000);
        mmio_base = NULL;
    }

    nic_present = 0;
    kprintf("[e1000] NIC shut down\n");
}

#ifdef MODULE
/* Module entry/exit points — the ELF loader looks for these symbols */
int init_module(void) {
    return e1000_init();
}
void cleanup_module(void) {
    e1000_exit();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Intel PRO/1000 (E1000/E1000E) PCI Ethernet driver");
MODULE_ALIAS("pci:v00008086d0000100Esv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000100Fsv*sd*bc*sc*i*");
#endif /* MODULE */

int e1000_receive(void *buf, uint16_t max_len) {
    if (!nic_present) return -1;

    int idx = rx_cur;
    if (!(rx_descs[idx].status & RDESC_STA_DD))
        return 0; /* nothing to receive */

    /* Check for errors (CRC, alignment, etc.) */
    if (rx_descs[idx].errors != 0) {
        rx_descs[idx].status = 0;
        int old_cur = rx_cur;
        rx_cur = (rx_cur + 1) % NUM_RX_DESC;
        e1000_write(REG_RDT, old_cur);
        return -2; /* error — skip this packet */
    }

    /* Check End-Of-Packet: if not set, packet spans multiple descriptors
     * (jumbo frame) — drop it since we don't support reassembly. */
    if (!(rx_descs[idx].status & RDESC_STA_EOP)) {
        rx_descs[idx].status = 0;
        int old_cur = rx_cur;
        rx_cur = (rx_cur + 1) % NUM_RX_DESC;
        e1000_write(REG_RDT, old_cur);
        return -3;
    }

    uint16_t len = rx_descs[idx].length;
    if (len > max_len) len = max_len;
    memcpy(buf, rx_buffers[idx], len);

    rx_descs[idx].status = 0;
    int old_cur = rx_cur;
    rx_cur = (rx_cur + 1) % NUM_RX_DESC;
    e1000_write(REG_RDT, old_cur);

    return len;
}
