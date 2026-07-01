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
#include "err.h"
#include "crc.h"
#include "vlan.h"
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

/* ── Interrupt moderation registers ───────────────────────────────── */
/* RDTR  — Receive Delay Timer (per-queue): delays RX interrupt generation
 *         after receiving a packet, allowing more packets to arrive and
 *         be coalesced into a single interrupt.
 *         Value in microseconds (82540EM: 1-65535, 0 = no delay).
 * RADV  — Receive Absolute Delay (per-queue): absolute maximum time
 *         (microseconds) before an RX interrupt is generated, regardless
 *         of RDTR (prevents starvation on low-traffic flows).
 * TIDV  — Transmit Interrupt Delay Value (per-queue): delays TX completion
 *         interrupt after a descriptor is transmitted.
 * TADV  — Transmit Absolute Delay (per-queue): absolute maximum time
 *         before a TX completion interrupt fires.
 *
 * EITR  — Extended Interrupt Throttling Rate (82576+, per-queue).
 *         82576 has per-queue EITR registers that override the global
 *         ITR for each individual queue, enabling fine-grained per-queue
 *         interrupt moderation.  Same encoding as ITR (256 ns units).
 */
#define REG_RDTR(q)  (0x2820 + (q) * 0x400)  /* per-queue RX delay timer */
#define REG_RADV(q)  (0x282C + (q) * 0x400)  /* per-queue RX absolute delay */
#define REG_TIDV(q)  (0x3820 + (q) * 0x400)  /* per-queue TX delay timer */
#define REG_TADV(q)  (0x382C + (q) * 0x400)  /* per-queue TX absolute delay */
#define REG_EITR(q)  (0x01680 + (q) * 4)     /* per-queue EITR (82576) */

/* Default interrupt moderation values */
#define INTR_RDTR_DEFAULT  100    /* 100 us RX delay */
#define INTR_RADV_DEFAULT  500    /* 500 us absolute RX max */
#define INTR_TIDV_DEFAULT  250    /* 250 us TX delay */
#define INTR_TADV_DEFAULT  1000   /* 1 ms absolute TX max */
#define INTR_EITR_DEFAULT  ITR_BALANCED  /* same as global ITR */

/* ── Multi-queue / RSS registers (82574 specific) */
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
#define CTRL_SLU    (1U << 6)    /* Set Link Up */
#define CTRL_RST    (1U << 26)

/* RCTL bits */
#define RCTL_EN     (1U << 1)
#define RCTL_UPE    (1U << 3)    /* Unicast promisc */
#define RCTL_MPE    (1U << 4)    /* Multicast promisc */
#define RCTL_BAM    (1U << 15)   /* Broadcast accept */
#define RCTL_BSIZE_2048 0       /* buffer size 2048 */
#define RCTL_SECRC  (1U << 26)   /* Strip CRC */
#define RCTL_MQ     (1U << 24)   /* Multiple Queues (required for RSS) */

/* TCTL bits */
#define TCTL_EN     (1U << 1)
#define TCTL_PSP    (1U << 3)

/* CTRL VLAN bits */
#define CTRL_VME    (1U << 19)   /* VLAN Mode Enable */

/* VLAN registers */
#define REG_VET     0x5200       /* VLAN EtherType (default 0x8100) */
#define REG_VFTA    0x5600       /* VLAN Filter Table Array (128 dwords) */

/* VFTA layout: 128 dwords indexed by VID[11:5], bit within word by VID[4:0] */
#define VFTA_SIZE   128

/* RX descriptor special field when VME is enabled (VLAN stripping):
 *   bits [15:13] = PCP (Priority Code Point)
 *   bit  [12]   = DEI (Drop Eligible Indicator)
 *   bits [11:0]  = VID (VLAN Identifier)
 */

/* TX descriptor command bits */
#define TDESC_CMD_EOP  (1U << 0)
#define TDESC_CMD_IFCS (1U << 1)
#define TDESC_CMD_RS   (1U << 3)
#define TDESC_CMD_VLE  (1U << 6)  /* VLAN Insertion Enable */

/* TX/RX descriptor status */
#define TDESC_STA_DD   (1U << 0)
#define RDESC_STA_DD   (1U << 0)
#define RDESC_STA_EOP  (1U << 1)

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

    /* ── Per-queue interrupt moderation state ───────────────────── */
    uint32_t itr_value;    /* Current ITR/EITR value for this queue */
    uint32_t rdtr;         /* RX Delay Timer (us) */
    uint32_t radv;         /* RX Absolute Delay (us) */
    uint32_t tidv;         /* TX Interrupt Delay Value (us) */
    uint32_t tadv;         /* TX Absolute Delay Value (us) */
    uint32_t itr_pkt_count; /* Packet count for adaptive ITR sampling */
    uint64_t itr_last_tick; /* Last RDTSC tick for adaptive ITR */
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
static const uint32_t rss_key[4] = {
    0x6D5A56DA, 0xBF914C5B, 0xF4A3FCF1, 0x3BB06F25
};

/* Runtime RSS hash key (mutable — defaults to rss_key above, but can be
 * overridden via the RSS control API).  For multi-queue without per-queue
 * affinity, clients can leave this as the default. */
static uint32_t rss_key_runtime[4];

/* Current RSS hash type bitmask (default: TCP/IPv4 + IPv4) */
static uint32_t rss_hash_types = E1000_RSS_HASH_TCP_IPV4
                               | E1000_RSS_HASH_IPV4;

/* ITR adaptive state (per queue — queue 0 ITR used as global) */
static uint32_t itr_current = ITR_BALANCED;
static uint32_t itr_pkt_count = 0;
static uint64_t itr_last_tick = 0;
static int      itr_enabled = 0;

/* VLAN offload state */
static int      vlan_offload_enabled = 0;

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
static void e1000_rss_write_key(const uint32_t key[4]) {
    if (!is_82574 && !is_82576) return;
    for (int i = 0; i < 4; i++)
        e1000_write(REG_RSSRK(i), key[i]);
}

/* Write the RSS Redirection Table.
 * RETA has 32 dwords, each encoding 4 entries (bytes).
 * Entry i maps to queue (i % num_queues) for a simple round-robin
 * distribution across available queues. */
static void e1000_rss_write_reta(void) {
    if (!is_82574 && !is_82576) return;
    uint32_t reta[32] = {0};

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

/* ── Toeplitz hash (software RSS hash) ─────────────────────────────── */

/* e1000_rss_toeplitz - Compute the Toeplitz hash for RSS.
 * @key:  40-byte RSS key (the 4 dword key, expanded byte-wise).
 * @data: input data to hash (typically a 4-tuple of IP+port).
 * @len:  length of input data in bytes.
 *
 * Returns the 32-bit RSS hash value.
 *
 * The Toeplitz hash is computed by XORing the input data bits with
 * the key bits in a Toeplitz matrix arrangement.  The algorithm below
 * matches the hardware RSS hash computation for IPv4/TCP tuples. */
static uint32_t e1000_rss_toeplitz(const uint8_t key[40],
                                   const uint8_t *data, size_t len)
{
    uint32_t hash = 0;
    int i, j;

    for (i = 0; i < 32; i++) {
        uint8_t bit = 0;
        for (j = 0; j < (int)len * 8; j++) {
            int data_byte = j / 8;
            int data_bit  = j % 8;
            int key_byte  = (i + j) / 8;
            int key_bit   = (i + j) % 8;

            if ((data[data_byte] >> (7 - data_bit)) & 1) {
                if ((key[key_byte] >> (7 - key_bit)) & 1) {
                    bit ^= 1;
                }
            }
        }
        hash = (hash << 1) | (bit & 1);
    }

    return hash;
}

/* e1000_rss_compute_ipv4_tcp - Compute the RSS hash for an IPv4/TCP packet.
 * @iph:  pointer to the IPv4 header (20 bytes).
 * @th:   pointer to the TCP header (after IP header).
 *
 * The input for the Toeplitz hash is a 4-tuple:
 *   src_addr (4 bytes) || dst_addr (4 bytes) || src_port (2 bytes) || dst_port (2 bytes)
 * This is 12 bytes total. */
static uint32_t e1000_rss_compute_ipv4_tcp(const uint8_t *iph,
                                           const uint8_t *th)
{
    uint8_t input[12];
    int i;

    /* src IP (4 bytes) */
    for (i = 0; i < 4; i++)
        input[i] = iph[12 + i];   /* offset 12 = src IP in IPv4 hdr */
    /* dst IP (4 bytes) */
    for (i = 0; i < 4; i++)
        input[4 + i] = iph[16 + i];
    /* src port (2 bytes, big-endian) */
    input[8] = th[0];
    input[9] = th[1];
    /* dst port (2 bytes) */
    input[10] = th[2];
    input[11] = th[3];

    /* Build 40-byte key from the 4 dword runtime key */
    uint8_t key_bytes[40];
    for (i = 0; i < 4; i++) {
        key_bytes[i * 4 + 0] = (uint8_t)( rss_key_runtime[i]        & 0xFF);
        key_bytes[i * 4 + 1] = (uint8_t)((rss_key_runtime[i] >> 8)  & 0xFF);
        key_bytes[i * 4 + 2] = (uint8_t)((rss_key_runtime[i] >> 16) & 0xFF);
        key_bytes[i * 4 + 3] = (uint8_t)((rss_key_runtime[i] >> 24) & 0xFF);
    }
    /* Zero out remaining key bytes (for IPv6 support) */
    for (i = 16; i < 40; i++)
        key_bytes[i] = 0;

    return e1000_rss_toeplitz(key_bytes, input, sizeof(input));
}

/* e1000_rss_compute_ipv4 - Compute RSS hash for IPv4-only (non-TCP).
 * @iph:  pointer to the IPv4 header (20 bytes).
 *
 * Input: src_addr (4 bytes) || dst_addr (4 bytes) = 8 bytes. */
static uint32_t e1000_rss_compute_ipv4(const uint8_t *iph)
{
    uint8_t input[8];
    int i;

    for (i = 0; i < 4; i++)
        input[i] = iph[12 + i];   /* src IP */
    for (i = 0; i < 4; i++)
        input[4 + i] = iph[16 + i]; /* dst IP */

    uint8_t key_bytes[40];
    for (i = 0; i < 4; i++) {
        key_bytes[i * 4 + 0] = (uint8_t)( rss_key_runtime[i]        & 0xFF);
        key_bytes[i * 4 + 1] = (uint8_t)((rss_key_runtime[i] >> 8)  & 0xFF);
        key_bytes[i * 4 + 2] = (uint8_t)((rss_key_runtime[i] >> 16) & 0xFF);
        key_bytes[i * 4 + 3] = (uint8_t)((rss_key_runtime[i] >> 24) & 0xFF);
    }
    for (i = 16; i < 40; i++)
        key_bytes[i] = 0;

    return e1000_rss_toeplitz(key_bytes, input, sizeof(input));
}

/* Forward declaration for e1000_rss_init (defined later) */
static void e1000_rss_init(void);

/* ── RSS control API ───────────────────────────────────────────────── */

/* e1000_rss_set_key - Override the runtime RSS hash key.
 * @key: 4 dwords (128 bits total) of the new RSS key.
 *
 * The new key is written to hardware immediately and also used for
 * any subsequent software hash computation. */
void e1000_rss_set_key(const uint32_t key[4])
{
    int i;
    for (i = 0; i < 4; i++)
        rss_key_runtime[i] = key[i];

    if (nic_present && (is_82574 || is_82576))
        e1000_rss_write_key(rss_key_runtime);

    kprintf("[E1000] RSS key updated\\n");
}

/* e1000_rss_get_key - Read back the current runtime RSS key.
 * @key: output buffer for 4 dwords. */
void e1000_rss_get_key(uint32_t key[4])
{
    int i;
    for (i = 0; i < 4; i++)
        key[i] = rss_key_runtime[i];
}

/* e1000_rss_set_hash_types - Set which RSS hash types are enabled.
 * @types: bitmask of E1000_RSS_HASH_* values.
 *
 * Re-programs the MRQC register if the NIC is active. */
int e1000_rss_set_hash_types(uint32_t types)
{
    /* Validate: at least one hash type */
    uint32_t valid = E1000_RSS_HASH_TCP_IPV4 | E1000_RSS_HASH_IPV4
                   | E1000_RSS_HASH_TCP_IPV6 | E1000_RSS_HASH_IPV6
                   | E1000_RSS_HASH_IPV6_EX | E1000_RSS_HASH_TCP_IPV6_EX;
    if (types & ~valid)
        return -EINVAL;

    rss_hash_types = types & valid;

    /* Re-init RSS on hardware if NIC is active.
     * This re-writes MRQC with the new hash type mask. */
    if (nic_present && (is_82574 || is_82576))
        e1000_rss_init();

    kprintf("[E1000] RSS hash types set to 0x%x\\n", (unsigned)rss_hash_types);
    return 0;
}

/* e1000_rss_get_hash_types - Read current RSS hash type bitmask. */
uint32_t e1000_rss_get_hash_types(void)
{
    return rss_hash_types;
}

/* e1000_rss_set_reta - Re-program the RSS Redirection Table.
 * @table: array of 128 uint8_t entries, each giving the target queue
 *         (0..num_queues-1) for the corresponding RETA entry.
 *
 * If @table is NULL, reconstructs a round-robin mapping
 * (same as e1000_rss_write_reta). */
int e1000_rss_set_reta(const uint8_t table[128])
{
    if (!nic_present || (!is_82574 && !is_82576))
        return -EIO;

    uint32_t reta[32] = {0};
    int i;

    if (table == NULL) {
        /* Round-robin across available queues */
        for (i = 0; i < 128; i++) {
            int word = i / 4;
            int shift = (i % 4) * 8;
            uint8_t q = (uint8_t)(i % num_queues);
            reta[word] &= ~(0xFFu << shift);
            reta[word] |= ((uint32_t)q << shift);
        }
    } else {
        /* Use caller-provided mapping */
        for (i = 0; i < 128; i++) {
            int word = i / 4;
            int shift = (i % 4) * 8;
            uint8_t q = table[i];
            if (q >= (uint8_t)num_queues)
                q = (uint8_t)(q % (uint8_t)num_queues);
            reta[word] &= ~(0xFFu << shift);
            reta[word] |= ((uint32_t)q << shift);
        }
    }

    for (i = 0; i < 32; i++)
        e1000_write(REG_RETA(i), reta[i]);

    kprintf("[E1000] RSS redirection table updated\\n");
    return 0;
}

/* ── RSS receive side helpers ──────────────────────────────────────── */

/* e1000_rss_get_rx_queue - Determine which RX queue a packet was delivered to,
 * based on the RSS hash queue mapping stored in the RETA.
 *
 * For the legacy receive path, we compute the queue from the packet's
 * IP/TCP headers using software RSS, matching what the hardware did.
 *
 * Returns the queue index (0..num_queues-1). */
static int e1000_rss_get_rx_queue(const uint8_t *buf, uint16_t len)
{
    if (num_queues <= 1 || !nic_present)
        return 0;

    if (len < 34)  /* minimum: eth(14) + IPv4(20) */
        return 0;

    /* Check for IPv4 ethertype */
    if (buf[12] != 0x08 || buf[13] != 0x00)
        return 0;

    const uint8_t *iph = buf + 14;
    uint8_t ip_proto = iph[9];
    uint32_t hash = 0;

    if (ip_proto == 6 && len >= 54) {
        /* TCP - use 4-tuple hash */
        const uint8_t *th = iph + 20;
        hash = e1000_rss_compute_ipv4_tcp(iph, th);
    } else {
        /* Non-TCP IPv4 - use 2-tuple hash */
        hash = e1000_rss_compute_ipv4(iph);
    }

    /* Map hash to queue via RETA (entries 0-127, hash >> 24 selects entry) */
    uint32_t reta_idx = (hash >> 24) & 0x7F;
    if (reta_idx >= 128) reta_idx = 0;

    /* Compute which queue the RETA maps this entry to (round-robin by default) */
    return (int)(reta_idx % (uint32_t)num_queues);
}

/* e1000_rss_get_queue_hash - Compute the RSS hash for a received packet
 * and the queue it maps to.
 * @buf: received Ethernet frame.
 * @len: frame length.
 * @hash_out: output pointer for the 32-bit RSS hash (may be NULL).
 *
 * Returns the target queue index from the RETA. */
int e1000_rss_get_queue_hash(const uint8_t *buf, uint16_t len,
                              uint32_t *hash_out)
{
    uint32_t hash = 0;
    int q = 0;

    if (num_queues <= 1 || !nic_present) {
        if (hash_out) *hash_out = 0;
        return 0;
    }

    if (len < 34)
        goto done;

    /* Check for IPv4 ethertype */
    if (buf[12] != 0x08 || buf[13] != 0x00)
        goto done;

    const uint8_t *iph = buf + 14;
    uint8_t ip_proto = iph[9];

    if (ip_proto == 6 && len >= 54) {
        /* TCP */
        const uint8_t *th = iph + 20;
        hash = e1000_rss_compute_ipv4_tcp(iph, th);
    } else {
        /* Non-TCP IPv4 */
        hash = e1000_rss_compute_ipv4(iph);
    }

done:
    uint32_t reta_idx = (hash >> 24) & 0x7F;
    if (reta_idx >= 128) reta_idx = 0;
    q = (int)(reta_idx % (uint32_t)num_queues);

    if (hash_out) *hash_out = hash;
    return q;
}

/* Configure RSS on hardware that supports it (82574, 82576).
 * Enables RSS with TCP/IPv4 and IPv4 hashing.
 *
 * MRQC (Multiple Receive Queues Command, 0x5818):
 *   82574: bits[1:0] = number of queues - 1 (0=1q, 1=2q),
 *          bits[2]   = HASH_IPV4,
 *          bits[3]   = HASH_TCP_IPV4,
 *          bit[7]    = RSS enable.
 *   82576: bits[2:0] = number of queues - 1,
 *          bits[6:4] = RSS hash type. */
static void e1000_rss_init(void) {
    if (!is_82574 && !is_82576) return;

    uint32_t mrqc = 0;
    int nq = num_queues;

    if (nq < 1) nq = 1;
    if (nq > E1000_MAX_QUEUES) nq = E1000_MAX_QUEUES;

    /* Write RSS key (use runtime key which is initialised to default) */
    e1000_rss_write_key(rss_key_runtime);

    /* Write RETA (redirection table) */
    e1000_rss_write_reta();

    if (is_82576) {
        /* 82576 MRQC:
         *   bits[2:0] = (num_queues - 1) & 7
         *   bits[6:4] = hash type mask
         *     bit 4 = HASH_IPV4
         *     bit 5 = HASH_TCP_IPV4
         *     bit 6 = HASH_IPV6 / TCP_IPV6 */
        uint32_t nq_bits = (uint32_t)(nq - 1) & 0x07;
        mrqc = nq_bits;

        /* Hash types: select based on rss_hash_types bitmask.
         * 82576 maps hash types to bits[6:4] differently from 82574. */
        if (rss_hash_types & E1000_RSS_HASH_IPV4)
            mrqc |= (1U << 4);
        if (rss_hash_types & E1000_RSS_HASH_TCP_IPV4)
            mrqc |= (1U << 5);
        if (rss_hash_types & (E1000_RSS_HASH_IPV6 | E1000_RSS_HASH_TCP_IPV6))
            mrqc |= (1U << 6);
    } else {
        /* 82574 MRQC:
         *   bit[0]  = RSS enable
         *   bit[1]  = HASH_IPV4
         *   bit[2]  = HASH_TCP_IPV4
         *   bit[3]  = HASH_IPV6
         *   bit[4]  = HASH_TCP_IPV6 */
        mrqc = 1;  /* RSS enable */

        if (rss_hash_types & E1000_RSS_HASH_IPV4)
            mrqc |= (1U << 1);
        if (rss_hash_types & E1000_RSS_HASH_TCP_IPV4)
            mrqc |= (1U << 2);
        if (rss_hash_types & E1000_RSS_HASH_IPV6)
            mrqc |= (1U << 3);
        if (rss_hash_types & E1000_RSS_HASH_TCP_IPV6)
            mrqc |= (1U << 4);
    }

    e1000_write(REG_MRQC, mrqc);

    kprintf("  e1000: RSS enabled (%d queues, MRQC=0x%x)\\n", nq, (unsigned)mrqc);
}

/* ── VLAN offload helpers ────────────────────────────────────────────── */

/*
 * e1000_vfta_set - Set a VLAN ID bit in the VFTA (VLAN Filter Table Array).
 * @vid: VLAN identifier (0-4095).
 *
 * The VFTA is a 128-dword bit array.  Each bit corresponds to one VLAN.
 * Register index = (vid >> 5), bit within register = (vid & 0x1F).
 */
static void e1000_vfta_set(uint16_t vid)
{
    uint32_t reg_idx = ((uint32_t)vid >> 5) & 0x7F;
    uint32_t bit     = (uint32_t)vid & 0x1F;
    uint32_t val;

    val = e1000_read(REG_VFTA + reg_idx * 4);
    val |= (1U << bit);
    e1000_write(REG_VFTA + reg_idx * 4, val);
}

/*
 * e1000_vfta_clear_vid - Clear a VLAN ID bit in the VFTA.
 * @vid: VLAN identifier (0-4095).
 */
static void e1000_vfta_clear_vid(uint16_t vid)
{
    uint32_t reg_idx = ((uint32_t)vid >> 5) & 0x7F;
    uint32_t bit     = (uint32_t)vid & 0x1F;
    uint32_t val;

    val = e1000_read(REG_VFTA + reg_idx * 4);
    val &= ~(1U << bit);
    e1000_write(REG_VFTA + reg_idx * 4, val);
}

/* e1000_vfta_clear - Zero out the entire VFTA (reject all VLANs). */
static void e1000_vfta_clear(void)
{
    for (int i = 0; i < VFTA_SIZE; i++)
        e1000_write(REG_VFTA + i * 4, 0);
}

/*
 * e1000_vlan_offload_init - Enable hardware VLAN offloading.
 *
 * Configures the NIC for hardware VLAN tag stripping on RX and tag
 * insertion on TX via the VLE descriptor bit.  Initialises the VLAN
 * filter table (VFTA) with the default VLAN ID.
 */
static void e1000_vlan_offload_init(void)
{
    if (!nic_present)
        return;

    /* Set VLAN EtherType (default 0x8100) */
    e1000_write(REG_VET, htons(VLAN_TPID));

    /* Clear VFTA and add default VLAN 1 */
    e1000_vfta_clear();
    e1000_vfta_set(VLAN_DEFAULT_VID);

    /* Set CTRL.VME — enable VLAN Mode.
     * When VME is set:
     *  - RX: hardware strips the 4-byte VLAN tag, writes it to the
     *    RX descriptor's special field.
     *  - TX: when the VLE bit is set in the TX descriptor's cmd byte,
     *    hardware inserts the VLAN tag from the special field. */
    {
        uint32_t ctrl = e1000_read(REG_CTRL);
        ctrl |= CTRL_VME;
        e1000_write(REG_CTRL, ctrl);
    }

    vlan_offload_enabled = 1;

    kprintf("  e1000: VLAN offloading enabled (VET=0x%04x, default VID=1)\n",
            VLAN_TPID);
}

/*
 * e1000_vlan_rx_add_vid - Add a VLAN ID to the hardware filter table.
 * @dev: net_device (unused).
 * @vid: VLAN identifier to add.
 *
 * Programs one entry in the VFTA so the NIC accepts frames tagged with
 * this VLAN.
 *
 * Return: 0 on success, negative errno on error.
 */
int e1000_vlan_rx_add_vid(struct net_device *dev, uint16_t vid)
{
    (void)dev;

    if (!nic_present)
        return -EIO;
    if (vid >= VLAN_MAX_VID)
        return -EINVAL;

    e1000_vfta_set(vid);
    kprintf("[E1000] VLAN %u added to filter table\n", (unsigned)vid);
    return 0;
}

/*
 * e1000_vlan_rx_kill_vid - Remove a VLAN ID from the hardware filter table.
 * @dev: net_device (unused).
 * @vid: VLAN identifier to remove.
 *
 * Clears the corresponding bit in the VFTA.  The default VLAN 1 cannot
 * be removed.
 *
 * Return: 0 on success, negative errno on error.
 */
int e1000_vlan_rx_kill_vid(struct net_device *dev, uint16_t vid)
{
    (void)dev;

    if (!nic_present)
        return -EIO;
    if (vid >= VLAN_MAX_VID)
        return -EINVAL;
    if (vid == VLAN_DEFAULT_VID)
        return -EINVAL;  /* cannot remove default VLAN */

    e1000_vfta_clear_vid(vid);
    kprintf("[E1000] VLAN %u removed from filter table\n", (unsigned)vid);
    return 0;
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

/* ── Per-queue interrupt moderation initialization ────────────────── */

/* e1000_intr_mod_queue_init - Initialize per-queue interrupt moderation
 * for a single queue.  Programs RDTR, RADV, TIDV, TADV registers and,
 * on 82576, the per-queue EITR.
 * @q: queue index to initialise.
 *
 * On 82540EM, only queue 0 RDTR/RADV/TIDV/TADV registers exist.
 * On 82574L, queues 0 and 1 have these registers.
 * On 82576, all 4 queues have these registers plus per-queue EITR. */
static void e1000_intr_mod_queue_init(int q)
{
    struct e1000_queue *qp = &queues[q];

    if (!nic_present || q >= num_queues)
        return;

    /* Initialize per-queue moderation state with defaults */
    qp->itr_value = itr_current;
    qp->rdtr      = INTR_RDTR_DEFAULT;
    qp->radv      = INTR_RADV_DEFAULT;
    qp->tidv      = INTR_TIDV_DEFAULT;
    qp->tadv      = INTR_TADV_DEFAULT;

    /* Only program RDTR/RADV on hardware that supports them.
     * 82540EM supports these only for queue 0.
     * 82574L supports them for queues 0 and 1.
     * 82576 supports them for all queues. */
    if (q == 0 || is_82574 || is_82576) {
        e1000_write(REG_RDTR(q), qp->rdtr);
        e1000_write(REG_RADV(q), qp->radv);
    }

    /* Same for TIDV/TADV */
    if (q == 0 || is_82574 || is_82576) {
        e1000_write(REG_TIDV(q), qp->tidv);
        e1000_write(REG_TADV(q), qp->tadv);
    }

    /* Program per-queue EITR on 82576 */
    if (is_82576) {
        e1000_write(REG_EITR(q), qp->itr_value);
    }

    kprintf("  e1000: queue %d intr_mod: ITR=%u RDTR=%u RADV=%u "
            "TIDV=%u TADV=%u\n",
            q, (unsigned)qp->itr_value,
            (unsigned)qp->rdtr, (unsigned)qp->radv,
            (unsigned)qp->tidv, (unsigned)qp->tadv);
}

/* e1000_intr_mod_init - Initialize interrupt moderation for all queues.
 * Called during e1000_init() after per-queue RX/TX rings are set up
 * and the global ITR has been configured. */
static void e1000_intr_mod_init(void)
{
    if (!nic_present)
        return;

    kprintf("  e1000: initializing per-queue interrupt moderation"
            " (%d queue(s))\n", num_queues);

    for (int q = 0; q < num_queues; q++)
        e1000_intr_mod_queue_init(q);

    kprintf("  e1000: interrupt moderation enabled"
            " (RDTR=%uus RADV=%uus TIDV=%uus TADV=%uus)\n",
            (unsigned)INTR_RDTR_DEFAULT, (unsigned)INTR_RADV_DEFAULT,
            (unsigned)INTR_TIDV_DEFAULT, (unsigned)INTR_TADV_DEFAULT);
}

/* ── Interrupt Moderation Public API ─────────────────────────────────── */

/* e1000_intr_mod_get_config - Return capabilities and current settings. */
int e1000_intr_mod_get_config(struct e1000_intr_mod_config *config)
{
    if (!config)
        return -EINVAL;
    if (!nic_present)
        return -EIO;

    memset(config, 0, sizeof(*config));

    config->capabilities = E1000_INTR_MOD_ITR | E1000_INTR_MOD_RDTR
                         | E1000_INTR_MOD_TIDV | E1000_INTR_MOD_ADAPTIVE;
    if (is_82576)
        config->capabilities |= E1000_INTR_MOD_EITR;

    config->itr_value = itr_current;
    config->rdtr      = INTR_RDTR_DEFAULT;
    config->radv      = INTR_RADV_DEFAULT;
    config->tidv      = INTR_TIDV_DEFAULT;
    config->tadv      = INTR_TADV_DEFAULT;

    return 0;
}

/* e1000_intr_mod_set_global_itr - Program the global ITR register.
 * @value: interval in 256 ns units (0 = off, 0xFFFF = max).
 *
 * On 82576+, writes the same value to all per-queue EITR registers
 * to maintain consistency. */
int e1000_intr_mod_set_global_itr(uint32_t value)
{
    if (!nic_present)
        return -EIO;

    if (value > 0xFFFF)
        value = 0xFFFF;

    e1000_set_itr(value);

    /* On 82576, also update per-queue EITR registers and state */
    if (is_82576) {
        for (int q = 0; q < num_queues; q++) {
            e1000_write(REG_EITR(q), value);
            queues[q].itr_value = value;
        }
    }

    kprintf("[E1000] Global ITR set to %u (256ns units, ~%u int/s)\n",
            (unsigned)value,
            (unsigned)(1000000000ULL / ((uint64_t)value * 256ULL + 1)));
    return 0;
}

/* e1000_intr_mod_get_global_itr - Return current global ITR value. */
uint32_t e1000_intr_mod_get_global_itr(void)
{
    if (!nic_present)
        return 0;
    return itr_current;
}

/* e1000_intr_mod_set_queue_itr - Set per-queue EITR on 82576.
 * @q: queue index.
 * @value: EITR interval in 256 ns units. */
int e1000_intr_mod_set_queue_itr(int q, uint32_t value)
{
    if (!nic_present)
        return -EIO;
    if (q < 0 || q >= num_queues)
        return -EINVAL;
    if (!is_82576)
        return -EOPNOTSUPP;  /* per-queue EITR requires 82576 */

    if (value > 0xFFFF)
        value = 0xFFFF;

    e1000_write(REG_EITR(q), value);
    queues[q].itr_value = value;

    kprintf("[E1000] Queue %d EITR set to %u (256ns units, ~%u int/s)\n",
            q, (unsigned)value,
            (unsigned)(1000000000ULL / ((uint64_t)value * 256ULL + 1)));
    return 0;
}

/* e1000_intr_mod_set_adaptive - Enable or disable adaptive ITR. */
int e1000_intr_mod_set_adaptive(int enable)
{
    if (!nic_present)
        return -EIO;

    itr_enabled = !!enable;
    kprintf("[E1000] Adaptive interrupt moderation %s\n",
            itr_enabled ? "enabled" : "disabled");
    return 0;
}

/* e1000_intr_mod_is_adaptive - Return adaptive ITR state. */
int e1000_intr_mod_is_adaptive(void)
{
    return itr_enabled ? 1 : 0;
}

/* e1000_intr_mod_dump - Log current interrupt moderation configuration. */
void e1000_intr_mod_dump(void)
{
    if (!nic_present) {
        kprintf("[E1000] NIC not present\n");
        return;
    }

    kprintf("[E1000] Interrupt Moderation Configuration:\n");
    kprintf("  Global ITR: %u (256ns units) ~%u int/s%s\n",
            (unsigned)itr_current,
            (unsigned)(1000000000ULL / ((uint64_t)itr_current * 256ULL + 1)),
            itr_enabled ? " [adaptive]" : "");

    if (is_82576) {
        for (int q = 0; q < num_queues; q++) {
            uint32_t eitr = e1000_read(REG_EITR(q));
            kprintf("  Queue %d EITR: %u ~%u int/s\n",
                    q, (unsigned)eitr,
                    (unsigned)(1000000000ULL / ((uint64_t)eitr * 256ULL + 1)));
        }
    }

    for (int q = 0; q < num_queues; q++) {
        uint32_t rdtr = e1000_read(REG_RDTR(q));
        uint32_t radv = e1000_read(REG_RADV(q));
        uint32_t tidv = e1000_read(REG_TIDV(q));
        uint32_t tadv = e1000_read(REG_TADV(q));
        kprintf("  Queue %d: RDTR=%uus RADV=%uus TIDV=%uus TADV=%uus\n",
                q, (unsigned)rdtr, (unsigned)radv,
                (unsigned)tidv, (unsigned)tadv);
    }
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
    if (IS_ERR((const void *)mmio_base)) {
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
        queues[q].itr_value = 0;
        queues[q].rdtr = 0;
        queues[q].radv = 0;
        queues[q].tidv = 0;
        queues[q].tadv = 0;
        queues[q].itr_pkt_count = 0;
        queues[q].itr_last_tick = 0;
    }

    /* Copy default RSS key into runtime key */
    for (int i = 0; i < 4; i++)
        rss_key_runtime[i] = rss_key[i];

    /* Configure per-queue interrupt vectors via IVAR (82574/82576).
     * Each queue gets its own MSI-X-style vector if available,
     * otherwise they all share the legacy IRQ.
     *
     * On 82576 with MSI-X, each queue can interrupt on a separate CPU,
     * enabling true RSS interrupt steering.
     * On 82574 (2 queues), we map both queues to the same legacy IRQ
     * unless MSI-X is available. */
    if (is_82574 || is_82576) {
        int have_msix = (apic_is_init_complete() != 0);  /* MSI-X requires APIC */

        for (int q = 0; q < num_queues; q++) {
            uint32_t ivar_val;
            int reg_idx = q / 2;  /* two entries per IVAR register */
            int byte_shift = (q % 2) * 8;

            if (have_msix) {
                /* Use a unique vector per queue for MSI-X.
                 * Vector pool: start at a safe offset from the legacy IRQ.
                 * On real hardware this would be allocated from the MSI-X
                 * vector allocator. */
                int vector = e1000_irq_line + q;
                if (vector > 23) vector = 23;  /* stay within ISA-safe range */
                queues[q].irq_vector = vector;
                ivar_val = IVAR_ENTRY(vector);
            } else {
                /* Legacy: all queues share the same IRQ */
                queues[q].irq_vector = -1;
                ivar_val = IVAR_ENTRY(e1000_irq_line);
            }

            uint32_t ivar_reg = e1000_read(REG_IVAR + reg_idx * 4);
            ivar_reg &= ~(0xFFu << byte_shift);
            ivar_reg |= (ivar_val << byte_shift);
            e1000_write(REG_IVAR + reg_idx * 4, ivar_reg);
        }

        kprintf("  e1000: IVAR configured for %d queue(s)%s\\n",
                num_queues,
                have_msix ? " (MSI-X per-queue)" : " (shared IRQ)");
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

    /* Initialize per-queue interrupt moderation (RDTR, RADV, TIDV, TADV, EITR) */
    e1000_intr_mod_init();

    /* Configure VLAN offloading (VME, VFTA, VET) */
    e1000_vlan_offload_init();

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
            ndev.flags    = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
            ndev.priv     = NULL;

            /* Wire up multicast and feature-change callbacks */
            ndev.set_multicast_list = (void *)(void *)e1000_set_multicast;
            ndev.set_features = NULL;  /* not yet used */

            /* VLAN offload callbacks */
            ndev.vlan_rx_add_vid  = e1000_vlan_rx_add_vid;
            ndev.vlan_rx_kill_vid = e1000_vlan_rx_kill_vid;

            /* Advertise VLAN offload capabilities */
            ndev.features = NETIF_F_HW_VLAN_CTAG_TX
                          | NETIF_F_HW_VLAN_CTAG_RX
                          | NETIF_F_HW_VLAN_CTAG_FILTER;

            int ifindex = netif_register(&ndev);
            if (ifindex >= 0) {
                kprintf("[E1000] registered as netdevice ifindex=%d\n", ifindex);
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

    kprintf("[DBG] e1000_netdev_transmit: len=%u\n", len);

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
        kprintf("[DBG] e1000_netdev_transmit: TIMEOUT waiting for desc %d\n", idx);
        qp->stats.tx_errors++;
        return -1;
    }

    uint64_t __e1k_flags;
    spinlock_irqsave_acquire(&e1000_lock, &__e1k_flags);

    const uint8_t *tx_data = data;
    uint16_t tx_len = len;
    uint8_t vlan_buf[RX_BUF_SIZE];  /* for VLAN-offloaded frame rebuild */

    /* VLAN offload: detect 802.1Q tagged frames and offload tag
     * insertion to hardware via the VLE (VLAN Insertion Enable) bit.
     *
     * When a frame has TPID 0x8100 at bytes 12-13, a 4-byte VLAN
     * header follows (TPID + TCI).  We strip these 4 bytes from the
     * frame body and write the TCI into the descriptor's special field
     * with VLE set — the hardware re-inserts the tag on the wire. */
    if (vlan_offload_enabled && len >= 18 &&
        data[12] == 0x81 && data[13] == 0x00) {
        uint16_t vlan_tci;

        /* Extract TCI from original frame (bytes 14-15) */
        vlan_tci = ((uint16_t)data[14] << 8) | data[15];

        /* Rebuild frame without the 4-byte VLAN header:
         *   bytes 0-11: DA + SA (unchanged)
         *   bytes 12+:  ethertype (was bytes 16-17) + payload */
        memcpy(vlan_buf, data, 12);
        if (len > 16)
            memcpy(vlan_buf + 12, data + 16, len - 16);
        tx_len = len - 4;
        tx_data = vlan_buf;

        /* Set VLE bit and write VLAN TCI into special field */
        qp->tx_descs[idx].special = vlan_tci;
        qp->tx_descs[idx].cmd = TDESC_CMD_EOP | TDESC_CMD_IFCS
                              | TDESC_CMD_RS | TDESC_CMD_VLE;
    } else {
        qp->tx_descs[idx].special = 0;
        qp->tx_descs[idx].cmd = TDESC_CMD_EOP | TDESC_CMD_IFCS | TDESC_CMD_RS;
    }

    memcpy(qp->tx_buffers[idx], tx_data, tx_len);
    qp->tx_descs[idx].length = tx_len;
    qp->tx_descs[idx].status = 0;

    qp->tx_cur = (qp->tx_cur + 1) % NUM_TX_DESC;
    e1000_q_write_tx(tx_q, 4, qp->tx_cur); /* TDT */

    spinlock_irqsave_release(&e1000_lock, __e1k_flags);

    /* Update per-queue transmit stats */
    qp->stats.tx_packets++;
    qp->stats.tx_bytes += len;

    kprintf("[DBG] e1000_netdev_transmit: done, new tx_cur=%u\n", qp->tx_cur);
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

        /* If RSS is active, compute the software RSS hash for logging */
        if (num_queues > 1) {
            uint32_t rss_hash = 0;
            int rss_q = e1000_rss_get_queue_hash(buf, len, &rss_hash);
            (void)rss_q;
            kprintf("[E1000] RX: queue=%d len=%u RSS_hash=0x%08x "
                    "RETA_idx=%u\\n",
                    rss_q, (unsigned)len, (unsigned)rss_hash,
                    (unsigned)((rss_hash >> 24) & 0x7F));
        }

        /* If VLAN offload is enabled, the hardware may have stripped a
         * VLAN tag from the frame and placed it in the RX descriptor's
         * special field.  Log extracted VLAN info for debugging. */
        if (vlan_offload_enabled) {
            uint16_t rx_vlan_tag = qp->rx_descs[idx].special;
            if (rx_vlan_tag != 0) {
                uint16_t rx_vid = rx_vlan_tag & VLAN_VID_MASK;
                (void)rx_vid;
                kprintf("[E1000] RX: VLAN tag stripped, VID=%u "
                        "PCP=%u DEI=%u\n",
                        (unsigned)rx_vid,
                        (unsigned)((rx_vlan_tag >> 13) & 7),
                        (unsigned)((rx_vlan_tag >> 12) & 1));
            }
        }

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

    kprintf("[E1000] Shutting down NIC...\n");

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

    kprintf("[E1000] NIC shut down\n");
}

#ifdef MODULE
int init_module(void) {
    return e1000_init();
}
void cleanup_module(void) {
    e1000_exit();
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
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

    kprintf("[E1000] Opening NIC...\n");

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

    kprintf("[E1000] NIC opened\n");
    return 0;
}

/* ── e1000_stop: Disable RX/TX, mask interrupts ──────────── */
int e1000_stop(void *dev)
{
    (void)dev;
    if (!nic_present) return -EIO;

    kprintf("[E1000] Stopping NIC...\n");

    /* Mask all interrupts */
    e1000_write(REG_IMC, 0xFFFFFFFF);

    /* Disable TX and RX */
    e1000_write(REG_TCTL, 0);
    e1000_write(REG_RCTL, 0);

    kprintf("[E1000] NIC stopped\n");
    return 0;
}

/* ── Multicast hash filtering (MTA) ──────────────────────────────── */

/*
 * e1000_hash_mc_addr - Compute 12-bit hash for multicast MAC address.
 * @addr: 6-byte destination MAC address (should be multicast).
 *
 * The e1000 uses CRC-32 over the 6-byte MAC to produce a 12-bit index
 * into the 128-dword Multicast Table Array (MTA).  The hash matches
 * the computation performed in QEMU's e1000 emulation.
 *
 * Return: hash value where bits [11:5] select the MTA register (0-127)
 *         and bits [4:0] select the bit within that register (0-31).
 */
static uint32_t e1000_hash_mc_addr(const uint8_t *addr)
{
	uint32_t hash = crc32(~0U, addr, 6);
	return hash;
}

/*
 * e1000_mta_add - Program one multicast MAC address into the MTA.
 * @addr: 6-byte destination MAC address.
 *
 * Computes the hash and sets the corresponding bit in the hardware
 * Multicast Table Array.
 */
static void e1000_mta_add(const uint8_t *addr)
{
	uint32_t hash = e1000_hash_mc_addr(addr);
	uint32_t reg_idx = (hash >> 25) & 0x7F;
	uint32_t bit     = (hash >> 16) & 0x1F;
	uint32_t val;

	val = e1000_read(REG_MTA + reg_idx * 4);
	val |= (1U << bit);
	e1000_write(REG_MTA + reg_idx * 4, val);
}

/*
 * e1000_mta_clear - Clear the entire Multicast Table Array.
 */
static void e1000_mta_clear(void)
{
	for (int i = 0; i < 128; i++)
		e1000_write(REG_MTA + i * 4, 0);
}

/* ── Promiscuous mode ─────────────────────────────────────────────── */

/*
 * e1000_set_promisc - Enable or disable unicast promiscuous mode.
 * @enable: non-zero to enable, 0 to disable.
 *
 * When enabled, the NIC accepts all unicast packets regardless of
 * destination MAC address (RCTL bit 3 = UPE).
 *
 * Return: 0 on success, negative errno on error.
 */
int e1000_set_promisc(int enable)
{
	if (!nic_present)
		return -EIO;

	uint32_t rctl = e1000_read(REG_RCTL);
	if (enable)
		rctl |= RCTL_UPE;
	else
		rctl &= ~RCTL_UPE;
	e1000_write(REG_RCTL, rctl);

	kprintf("[E1000] %s promiscuous mode\n",
		enable ? "enabled" : "disabled");
	return 0;
}

/*
 * e1000_set_allmulti - Enable or disable multicast-all mode.
 * @enable: non-zero to accept all multicast, 0 for hash-filtered.
 *
 * When enabled, the NIC accepts all multicast packets bypassing the
 * MTA filter (RCTL bit 4 = MPE).
 *
 * Return: 0 on success, negative errno on error.
 */
int e1000_set_allmulti(int enable)
{
	if (!nic_present)
		return -EIO;

	uint32_t rctl = e1000_read(REG_RCTL);
	if (enable)
		rctl |= RCTL_MPE;
	else
		rctl &= ~RCTL_MPE;
	e1000_write(REG_RCTL, rctl);

	kprintf("[E1000] all-multicast mode %s\n",
		enable ? "enabled" : "disabled");
	return 0;
}

/* ── e1000_set_multicast: Update multicast filters ──────────── */

/*
 * e1000_set_multicast - Program multicast address filter list.
 * @dev:   opaque device pointer (unused, for future compat).
 * @addr:  pointer to an array of 6-byte multicast MAC addresses
 *         (may be NULL if count == 0).
 * @count: number of addresses in the list.
 *
 * When a multicast address list is provided (count > 0), each address
 * is hashed into the MTA hash table.  If the list is empty, the MTA
 * is cleared and multicast-promiscuous mode is disabled.
 *
 * When more than MULTICAST_HASH_LIMIT addresses are present, falls
 * back to multicast-promiscuous mode (MPE) for simplicity.
 *
 * Return: 0 on success, negative errno on error.
 */
#define MULTICAST_HASH_LIMIT 16

int e1000_set_multicast(void *dev, void *addr, int count)
{
	(void)dev;

	if (!nic_present)
		return -EIO;

	if (count == 0) {
		/* Clear all multicast filters */
		e1000_mta_clear();
		e1000_set_allmulti(0);
		kprintf("[E1000] Multicast filter cleared\n");
		return 0;
	}

	/* For large lists, fall back to multicast-promiscuous mode */
	if (count > MULTICAST_HASH_LIMIT) {
		e1000_mta_clear();
		e1000_set_allmulti(1);
		kprintf("[E1000] Multicast list too large (%d), "
			"enabling all-multicast mode\n", count);
		return 0;
	}

	/* Program each address into the MTA hash table */
	e1000_mta_clear();

	const uint8_t (*mc_list)[6] = (const uint8_t (*)[6])addr;
	for (int i = 0; i < count; i++) {
		e1000_mta_add(mc_list[i]);
	}

	/* Disable multicast promiscuous mode — rely on MTA filtering */
	e1000_set_allmulti(0);

	kprintf("[E1000] Multicast filter set: %d address(es)\n", count);
	return 0;
}
