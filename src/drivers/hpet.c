/*
 * src/drivers/hpet.c — High Precision Event Timer (HPET) driver.
 *
 * Detects the HPET via ACPI-firmware-defined MMIO at 0xFED00000,
 * reads capabilities, and configures Timer 0 in periodic mode.
 * The kernel currently uses PIT for the system tick; HPET is
 * configured here so it is ready for the high-resolution timer
 * subsystem and as a fallback on HPET-only chipsets.
 *
 * Reference: Intel IA-PC HPET Specification (Revision 1.0a).
 */

#define KERNEL_INTERNAL
#include "hpet.h"
#include "printf.h"
#include "types.h"
#include "initcall.h"

/* ── HPET MMIO register offsets (relative to HPET_BASE) ────────────── */
#define HPET_GCAP_ID        0x000   /* General Capabilities & ID (64-bit) */
#define HPET_GEN_CONF       0x010   /* General Configuration (32-bit) */
#define HPET_GEN_STATUS     0x020   /* General Interrupt Status (32-bit) */
#define HPET_COUNTER        0x0F0   /* Main Counter (64-bit) */
#define HPET_T0_CONF        0x100   /* Timer 0 Configuration (64-bit) */
#define HPET_T0_COMPARATOR  0x108   /* Timer 0 Comparator (64-bit) */
#define HPET_T1_CONF        0x120   /* Timer 1 Configuration (64-bit) */
#define HPET_T1_COMPARATOR  0x128   /* Timer 1 Comparator (64-bit) */
#define HPET_T2_CONF        0x140   /* Timer 2 Configuration (64-bit) */
#define HPET_T2_COMPARATOR  0x148   /* Timer 2 Comparator (64-bit) */

/* General Configuration bits */
#define HPET_CFG_ENABLE     (1ULL << 0)   /* overall enable */
#define HPET_CFG_LEG_RT     (1ULL << 1)   /* legacy replacement route */

/* Timer N Configuration bits */
#define HPET_TN_ENABLE      (1ULL << 0)   /* timer enable */
#define HPET_TN_PERIODIC    (1ULL << 1)   /* 1 = periodic, 0 = one-shot */
#define HPET_TN_INT_ENABLE  (1ULL << 2)   /* interrupt enable */
#define HPET_TN_FSB_ENABLE  (1ULL << 4)   /* FSB / MSI interrupt enable */
#define HPET_TN_FSB_INT_DEL (1ULL << 5)   /* FSB interrupt delivery */
#define HPET_TN_32BIT       (1ULL << 8)   /* 1 = 32-bit, 0 = 64-bit (if supp.) */
#define HPET_TN_VAL_SET     (1ULL << 9)   /* periodic accumulator reload */

/* Standard HPET MMIO base address (x86 platform default) */
#define HPET_BASE 0xFED00000

/* Capability information extracted from GCAP_ID */
static int      hpet_present     = 0;
static uint32_t hpet_period_fs   = 0;   /* counter tick in femtoseconds */
static int      hpet_64bit       = 0;   /* counter can count to 2^64-1 */

/* ── MMIO helpers ─────────────────────────────────────────────────── */

static inline uint32_t hpet_read32(uint32_t reg)
{
    const volatile uint32_t *base =
        (const volatile uint32_t *)PHYS_TO_VIRT(HPET_BASE);
    return base[reg / 4];
}

static inline void hpet_write32(uint32_t reg, uint32_t val)
{
    volatile uint32_t *base =
        (volatile uint32_t *)PHYS_TO_VIRT(HPET_BASE);
    base[reg / 4] = val;
}

static inline uint64_t hpet_read64(uint32_t reg)
{
    const volatile uint64_t *base =
        (const volatile uint64_t *)PHYS_TO_VIRT(HPET_BASE);
    return base[reg / 8];
}

static inline void hpet_write64(uint32_t reg, uint64_t val)
{
    volatile uint64_t *base =
        (volatile uint64_t *)PHYS_TO_VIRT(HPET_BASE);
    base[reg / 8] = val;
}

/* ── Public API ───────────────────────────────────────────────────── */

int hpet_is_present(void)
{
    return hpet_present;
}

void __init hpet_init(void)
{
    uint64_t gcap_id;
    uint8_t  revision_id;
    uint16_t vendor_id;
    uint8_t  counter_size;
    uint32_t period;
    uint32_t num_timers;
    uint64_t freq_hz;

    /*
     * Read the General Capabilities & ID Register.
     * This 64-bit register is at offset 0x000 of the HPET MMIO region.
     * If the HPET is absent, the PCI/MMIO bus returns all-ones (0xFFFF).
     */
    gcap_id = hpet_read64(HPET_GCAP_ID);

    if ((uint32_t)(gcap_id & 0xFFFFFFFF) == 0xFFFFFFFF) {
        kprintf("[--] No HPET timer found at 0xFED00000\n");
        return;
    }

    hpet_present    = 1;
    revision_id     = (uint8_t)((gcap_id >> 16) & 0xFF);
    vendor_id       = (uint16_t)(gcap_id & 0xFFFF);
    counter_size    = (uint8_t)((gcap_id >> 13) & 1);   /* 0 = 32, 1 = 64 */
    period          = (uint32_t)(gcap_id >> 32);          /* femtoseconds */
    num_timers      = (uint32_t)((gcap_id >> 8) & 0x1F); /* count - 1 */

    hpet_period_fs  = period;
    hpet_64bit      = counter_size ? 1 : 0;

    kprintf("[OK] HPET detected: vendor=0x%04x rev=0x%02x "
            "%d timers counter=%d-bit period=%u fs\n",
            vendor_id, revision_id,
            num_timers + 1,
            hpet_64bit ? 64 : 32,
            period);

    if (period == 0) {
        kprintf("[WARN] HPET period is 0 — cannot configure timer\n");
        return;
    }

    /*
     * Compute the HPET counter frequency.
     *   period_fs       = number of femtoseconds per counter tick
     *   freq_hz         = 10^15 / period_fs  (since 1 Hz = 1 tick/s,
     *                      and 1 fs = 10^15 Hz)
     */
    freq_hz = 1000000000000000ULL / period;
    kprintf("[HPET] Main counter frequency: %llu Hz (period=%u fs)\n",
            freq_hz, period);

    /*
     * ── Configure Timer 0 for periodic mode ──────────────────────────
     *
     * Per the HPET spec, to set up a periodic timer:
     *   1. Disable the HPET globally (GEN_CONF.ENABLE = 0)
     *   2. Configure Timer N:
     *      - TN_PERIODIC (bit 1) = 1  (periodic mode)
     *      - TN_INT_ENABLE (bit 2) = 0 (interrupts off until consumer exists)
     *      - TN_ENABLE (bit 0) = 1
     *   3. Write the Timer N Comparator with the desired period
     *      (in counter ticks).  In periodic mode, the comparator value
     *      is automatically added to the accumulator on each match.
     *   4. Reset the main counter to 0.
     *   5. Enable the HPET globally (GEN_CONF.ENABLE = 1).
     *
     * We set the timer period to 1 second (freq_hz counter ticks).  This
     * is intentionally conservative: the PIT drives the main 100 Hz system
     * tick; the HPET is set up here so the high-resolution timer subsystem
     * (clockevent) can reprogram it to finer granularity later by writing
     * a new comparator value.
     */
    hpet_write32(HPET_GEN_CONF, 0);

    /* Clear any pending timer interrupt status */
    hpet_write32(HPET_GEN_STATUS, 1);

    /*
     * Configure Timer 0: periodic, no interrupt, enabled.
     * The interrupt is left disabled here because no consumer
     * (hrtimer / clockevent) has registered for it yet.
     * The HPET periodic mode itself is proved by reading back
     * the configuration register.
     */
    uint32_t t0_conf = HPET_TN_ENABLE | HPET_TN_PERIODIC;

    /* If the counter only supports 32-bit mode, set the 32-bit flag */
    if (!hpet_64bit)
        t0_conf |= HPET_TN_32BIT;

    hpet_write32(HPET_T0_CONF, t0_conf);

    /*
     * Set the Timer 0 period to 1 second worth of counter ticks.
     * In periodic mode the comparator value is the accumulator step:
     *   period_ticks = freq_hz  (i.e., 1 second in counter ticks).
     * Later, a clockevent consumer can reprogram it to a finer
     * granularity by writing a new comparator value.
     */
    hpet_write64(HPET_T0_COMPARATOR, freq_hz);

    /* Reset the main counter to start counting from 0 */
    hpet_write64(HPET_COUNTER, 0);

    /* Globally enable the HPET */
    hpet_write32(HPET_GEN_CONF, HPET_CFG_ENABLE);

    /*
     * Verify: read back the configuration to confirm the write took.
     * HPET_TN_PERIODIC (bit 1) and HPET_TN_ENABLE (bit 0) should be set.
     */
    uint32_t t0_conf_read = hpet_read32(HPET_T0_CONF);
    if ((t0_conf_read & (HPET_TN_ENABLE | HPET_TN_PERIODIC))
            == (HPET_TN_ENABLE | HPET_TN_PERIODIC)) {
        kprintf("[OK] HPET Timer 0 configured in periodic mode "
                "(freq=%llu Hz, period=%u fs)\n", freq_hz, period);
    } else {
        kprintf("[WARN] HPET Timer 0 config mismatch: "
                "wrote 0x%x, read 0x%x\n", t0_conf, t0_conf_read);
    }
}
device_initcall(hpet_init);
