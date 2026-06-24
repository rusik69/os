/*
 * spi.c — SPI bus controller framework
 *
 * Implements the SPI bus master/slave device model with support for:
 *   - Master registration and unregistration
 *   - Device registration with per-device mode/speed
 *   - Full-duplex transfers via spi_transfer()
 *   - Half-duplex via spi_write_then_read()
 *   - GPIO-based bit-bang SPI master for emulated/simple hardware
 *   - Module export for loadable driver modules
 *
 * Bit-bang implementation uses port-mapped I/O for clock (SCLK),
 * master-out-slave-in (MOSI), master-in-slave-out (MISO), and
 * chip-select (CS) lines — suitable for QEMU and simple platforms.
 */

#define KERNEL_INTERNAL
#include "spi.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "spinlock.h"
#include "export.h"

/* ── Lock protecting the master table ─────────────────────────────── */
static spinlock_t g_spi_lock;
static int g_spi_initialized = 0;

/* ── Registered SPI master controllers ────────────────────────────── */
static struct spi_master *g_masters[SPI_BUS_MAX];

/* ── Initialisation ────────────────────────────────────────────────── */
void __init spi_init(void)
{
    if (g_spi_initialized)
        return;

    spinlock_init(&g_spi_lock);
    memset(g_masters, 0, sizeof(g_masters));
    g_spi_initialized = 1;

    kprintf("[OK] SPI bus framework initialized (%d buses max, bitbang available)\n",
            SPI_BUS_MAX);
}

/* ── Master registration ──────────────────────────────────────────── */

int spi_master_register(int bus_id, const struct spi_master_ops *ops,
                        void *priv, int mode, uint32_t speed)
{
    if (!g_spi_initialized)
        return -1;
    if (bus_id < 0 || bus_id >= SPI_BUS_MAX)
        return -1;
    if (!ops || !ops->transfer_one)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_spi_lock, &irq_flags);

    /* Check if bus_id is already taken */
    if (g_masters[bus_id] != NULL) {
        spinlock_irqsave_release(&g_spi_lock, irq_flags);
        return -1;  /* EBUSY */
    }

    struct spi_master *master = (struct spi_master *)kmalloc(sizeof(struct spi_master));
    if (!master) {
        spinlock_irqsave_release(&g_spi_lock, irq_flags);
        return -1;  /* ENOMEM */
    }

    memset(master, 0, sizeof(struct spi_master));
    master->ops        = ops;
    master->priv       = priv;
    master->bus_id     = bus_id;
    master->default_mode  = mode;
    master->default_speed = speed;
    master->cs_active_low = 1;  /* standard active-low CS */

    g_masters[bus_id] = master;

    spinlock_irqsave_release(&g_spi_lock, irq_flags);

    kprintf("[SPI] Master registered on bus %d (mode=%d, speed=%u Hz)\n",
            bus_id, mode, (unsigned int)speed);
    return 0;
}

int spi_master_unregister(int bus_id)
{
    if (!g_spi_initialized)
        return -1;
    if (bus_id < 0 || bus_id >= SPI_BUS_MAX)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_spi_lock, &irq_flags);

    struct spi_master *master = g_masters[bus_id];
    if (!master) {
        spinlock_irqsave_release(&g_spi_lock, irq_flags);
        return -1;  /* ENODEV */
    }

    /* Check that no devices remain attached */
    if (master->device_count > 0) {
        spinlock_irqsave_release(&g_spi_lock, irq_flags);
        return -1;  /* EBUSY */
    }

    g_masters[bus_id] = NULL;
    kfree(master);

    spinlock_irqsave_release(&g_spi_lock, irq_flags);

    kprintf("[SPI] Master unregistered from bus %d\n", bus_id);
    return 0;
}

struct spi_master *spi_get_master(int bus_id)
{
    if (!g_spi_initialized || bus_id < 0 || bus_id >= SPI_BUS_MAX)
        return NULL;
    return g_masters[bus_id];
}

/* ── Device registration ──────────────────────────────────────────── */

struct spi_device *spi_device_register(struct spi_master *master,
                                       int cs_pin, int mode, uint32_t speed)
{
    if (!master)
        return NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_spi_lock, &irq_flags);

    if (master->device_count >= SPI_MAX_DEVICES) {
        spinlock_irqsave_release(&g_spi_lock, irq_flags);
        return NULL;
    }

    struct spi_device *dev = (struct spi_device *)kmalloc(sizeof(struct spi_device));
    if (!dev) {
        spinlock_irqsave_release(&g_spi_lock, irq_flags);
        return NULL;
    }

    memset(dev, 0, sizeof(struct spi_device));
    dev->master       = master;
    dev->cs_pin       = cs_pin;
    dev->mode         = (mode >= 0) ? mode : master->default_mode;
    dev->max_speed    = (speed > 0) ? speed : master->default_speed;
    dev->bit_order    = SPI_BIT_ORDER_MSB;
    dev->bits_per_word = 8;

    master->devices[master->device_count++] = dev;

    spinlock_irqsave_release(&g_spi_lock, irq_flags);

    kprintf("[SPI] Device registered on bus %d, CS=GPIO%d, mode=%d, speed=%u Hz\n",
            master->bus_id, cs_pin, dev->mode, (unsigned int)dev->max_speed);
    return dev;
}

void spi_device_unregister(struct spi_device *dev)
{
    if (!dev || !dev->master)
        return;

    struct spi_master *master = dev->master;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_spi_lock, &irq_flags);

    for (int i = 0; i < master->device_count; i++) {
        if (master->devices[i] == dev) {
            /* Compact the array */
            for (int j = i; j < master->device_count - 1; j++)
                master->devices[j] = master->devices[j + 1];
            master->device_count--;
            break;
        }
    }

    spinlock_irqsave_release(&g_spi_lock, irq_flags);

    kfree(dev);
}

/* ── Core transfer function ────────────────────────────────────────── */

int spi_transfer(struct spi_device *dev,
                 struct spi_transfer *transfers, int num)
{
    if (!dev || !dev->master || !transfers || num <= 0)
        return -1;  /* EINVAL */

    struct spi_master *master = dev->master;
    if (!master->ops || !master->ops->transfer_one)
        return -1;  /* ENOSYS */

    /* ── Configure SPI mode (CPHA, CPOL) ───────────────────────────
     * Apply the SPI mode to the controller before starting the
     * transfer.  Mode is encoded as:
     *   bits [1:0] = CPOL and CPHA from SPI_MODE_x constants
     *   bit  [2]   = CS high (vs active low)
     *   bit  [3]   = LSB first (vs MSB first)
     */

    /* Set CPOL (Clock Polarity) and CPHA (Clock Phase) from mode */
    if (master->ops->set_mode) {
        int ret = master->ops->set_mode(master->priv, dev->mode);
        if (ret < 0)
            return ret;
    }

    /* ── Configure word size (bits per word) ───────────────────────
     * The number of bits per word affects how data is serialized.
     * Standard is 8 bits per word.  Some devices support 16-bit
     * word sizes for higher throughput.
     */
    if (master->ops->set_word_size) {
        int ret = master->ops->set_word_size(master->priv, dev->bits_per_word);
        if (ret < 0) {
            /* Fall back to 8-bit if set_word_size fails */
            dev->bits_per_word = 8;
        }
    }

    if (master->ops->set_speed) {
        int ret = master->ops->set_speed(master->priv, dev->max_speed);
        if (ret < 0)
            return ret;
    }

    if (master->ops->set_bit_order) {
        int ret = master->ops->set_bit_order(master->priv, dev->bit_order);
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < num; i++) {
        struct spi_transfer *t = &transfers[i];

        if (t->len == 0)
            continue;

        /* Individual transfer speed override */
        uint32_t speed = (t->speed > 0) ? t->speed : dev->max_speed;
        if (speed != dev->max_speed && master->ops->set_speed) {
            int ret = master->ops->set_speed(master->priv, speed);
            if (ret < 0)
                return ret;
        }

        /* Execute the transfer */
        int ret = master->ops->transfer_one(master->priv,
                                            t->tx_buf,
                                            t->rx_buf,
                                            t->len,
                                            dev->cs_pin,
                                            master->cs_active_low);
        if (ret < 0)
            return ret;

        /* If cs_change is set, de-assert CS — the next transfer will
         * pick it up again if it re-asserts.  For the bitbang path,
         * each transfer_one already handles CS, so this is advisory. */
        if (t->cs_change) {
            /* De-assert CS: done implicitly by transfer_one if it drops CS.
             * Some controllers keep CS asserted between transfers;
             * the bitbang implementation always de-asserts after each
             * transfer_one, so this is a no-op for now. */
        }
    }

    return 0;
}

/* ── Write-then-read convenience function ───────────────────────────── */

int spi_write_then_read(struct spi_device *dev,
                        const uint8_t *tx_buf, uint32_t tx_len,
                        uint8_t *rx_buf, uint32_t rx_len)
{
    if (!dev || (!tx_buf && !rx_buf))
        return -1;
    if (tx_len == 0 && rx_len == 0)
        return 0;

    struct spi_transfer transfers[2];
    int num = 0;

    /* Write phase */
    if (tx_len > 0) {
        transfers[num].tx_buf    = tx_buf;
        transfers[num].rx_buf    = NULL;
        transfers[num].len       = tx_len;
        transfers[num].speed     = 0;
        transfers[num].cs_change = (rx_len == 0) ? 1 : 0;
        num++;
    }

    /* Read phase */
    if (rx_len > 0) {
        transfers[num].tx_buf    = NULL;
        transfers[num].rx_buf    = rx_buf;
        transfers[num].len       = rx_len;
        transfers[num].speed     = 0;
        transfers[num].cs_change = 1;
        num++;
    }

    return spi_transfer(dev, transfers, num);
}

/* ══════════════════════════════════════════════════════════════════════
 * ── GPIO-based Bit-bang SPI Master Implementation ────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * Bit-bang SPI uses 4 port-mapped GPIO pins:
 *   SCLK — Serial Clock (output)
 *   MOSI — Master Out, Slave In (output)
 *   MISO — Master In, Slave Out (input)
 *   CS   — Chip Select (output, active low)
 *
 * Each pin is controlled via a port address and bit position.
 */

struct bitbang_pin {
    uint16_t port;       /* I/O port address */
    int      bit;        /* bit position within the port */
};

struct bitbang_priv {
    struct bitbang_pin sclk;
    struct bitbang_pin mosi;
    struct bitbang_pin miso;
    struct bitbang_pin cs;
    int  mode;           /* current SPI mode */
    int  bit_order;      /* 0 = MSB first, 1 = LSB first */
    uint32_t speed;      /* nominal speed in Hz */
};

/* ── Bit-bang GPIO helpers ─────────────────────────────────────────── */

static inline void bb_pin_high(const struct bitbang_pin *p)
{
    uint8_t v = inb(p->port);
    outb(p->port, v | (uint8_t)(1U << p->bit));
}

static inline void bb_pin_low(const struct bitbang_pin *p)
{
    uint8_t v = inb(p->port);
    outb(p->port, v & (uint8_t)~(1U << p->bit));
}

static inline int bb_pin_read(const struct bitbang_pin *p)
{
    return (inb(p->port) >> p->bit) & 1;
}

/* ── Microsecond delay via io_wait (emulation-friendly) ────────────── */
static inline void bb_delay(void)
{
    io_wait();
    io_wait();
}

/* ── Set clock phase for half-period delay ─────────────────────────── */
static inline void bb_half_period(struct bitbang_priv *bp)
{
    /* crudely approximate a half-clock-period delay.
     * At 100 kHz, half period = 5 us — io_wait() * 2 gives ~1 us on QEMU.
     * For lower speeds we add more io_wait calls. */
    int count = (bp->speed < 100000) ? 8 : 2;
    for (int i = 0; i < count; i++)
        io_wait();
}

/* ── Transfer one byte in the configured mode ──────────────────────── */
static int bb_transfer_byte(struct bitbang_priv *bp, uint8_t tx, uint8_t *rx)
{
    uint8_t rx_val = 0;
    int cpol = (bp->mode >> 1) & 1;
    int cpha = bp->mode & 1;

    /*
     * SPI Mode Reference:
     *   Mode | CPOL | CPHA | Clock idle | Sample edge
     *   ─────┼──────┼──────┼────────────┼─────────────
     *    0   |  0   |  0   | LOW        | rising  (leading)
     *    1   |  0   |  1   | LOW        | falling (trailing)
     *    2   |  1   |  0   | HIGH       | falling (leading)
     *    3   |  1   |  1   | HIGH       | rising  (trailing)
     */

    /* Set clock idle level */
    if (cpol)
        bb_pin_high(&bp->sclk);
    else
        bb_pin_low(&bp->sclk);
    bb_half_period(bp);

    for (int i = 0; i < 8; i++) {
        int bit_idx = (bp->bit_order == SPI_BIT_ORDER_LSB) ? i : (7 - i);
        int tx_bit = (tx >> bit_idx) & 1;

        /* If CPHA=0: sample on leading edge (clock transitions away from idle) */
        if (cpha == 0) {
            /* Drive MOSI before clock edge */
            if (tx_bit)
                bb_pin_high(&bp->mosi);
            else
                bb_pin_low(&bp->mosi);
            bb_half_period(bp);

            /* Toggle clock: leading edge */
            if (cpol)
                bb_pin_low(&bp->sclk);
            else
                bb_pin_high(&bp->sclk);
            bb_half_period(bp);

            /* Sample MISO (input) */
            if (bb_pin_read(&bp->miso))
                rx_val |= (uint8_t)(1U << bit_idx);

            /* Toggle clock: trailing edge (back to idle) */
            if (cpol)
                bb_pin_high(&bp->sclk);
            else
                bb_pin_low(&bp->sclk);
        } else {
            /* CPHA=1: sample on trailing edge */
            /* Toggle clock: leading edge */
            if (cpol)
                bb_pin_low(&bp->sclk);
            else
                bb_pin_high(&bp->sclk);
            bb_half_period(bp);

            /* Drive MOSI (data valid after leading edge) */
            if (tx_bit)
                bb_pin_high(&bp->mosi);
            else
                bb_pin_low(&bp->mosi);
            bb_half_period(bp);

            /* Toggle clock: trailing edge — sample MISO */
            if (cpol)
                bb_pin_high(&bp->sclk);
            else
                bb_pin_low(&bp->sclk);

            if (bb_pin_read(&bp->miso))
                rx_val |= (uint8_t)(1U << bit_idx);

            bb_half_period(bp);
        }
    }

    if (rx)
        *rx = rx_val;
    return 0;
}

/* ── Bit-bang operations implementation ─────────────────────────────── */

static int bb_transfer_one(void *priv, const uint8_t *tx_buf, uint8_t *rx_buf,
                           uint32_t len, int cs_pin, int cs_active)
{
    (void)cs_pin;
    (void)cs_active;
    struct bitbang_priv *bp = (struct bitbang_priv *)priv;
    if (!bp || len == 0)
        return -1;  /* EINVAL */

    /* Assert CS (active low by default) — pull low */
    bb_pin_low(&bp->cs);
    bb_delay();

    /* Transfer bytes */
    for (uint32_t i = 0; i < len; i++) {
        uint8_t tx_byte = tx_buf ? tx_buf[i] : 0xFF;
        uint8_t rx_byte = 0;

        if (bb_transfer_byte(bp, tx_byte, &rx_byte) < 0) {
            bb_pin_high(&bp->cs);  /* de-assert on error */
            return -1;  /* EIO */
        }

        if (rx_buf)
            rx_buf[i] = rx_byte;
    }

    /* De-assert CS (back to inactive high) */
    bb_pin_high(&bp->cs);
    bb_delay();

    return 0;
}

static int bb_set_mode(void *priv, int mode)
{
    struct bitbang_priv *bp = (struct bitbang_priv *)priv;
    if (!bp)
        return -1;
    bp->mode = mode & SPI_MODE_MASK;
    return 0;
}

static int bb_set_speed(void *priv, uint32_t speed)
{
    struct bitbang_priv *bp = (struct bitbang_priv *)priv;
    if (!bp || speed == 0)
        return -1;
    bp->speed = speed;
    return 0;
}

static int bb_set_bit_order(void *priv, int lsb)
{
    struct bitbang_priv *bp = (struct bitbang_priv *)priv;
    if (!bp)
        return -1;
    bp->bit_order = lsb ? 1 : 0;
    return 0;
}

static int bb_set_word_size(void *priv, int bits)
{
    struct bitbang_priv *bp = (struct bitbang_priv *)priv;
    if (!bp)
        return -1;
    /* Bitbang only supports 8-bit words */
    if (bits != 8)
        return -1;
    return 0;
}

static const struct spi_master_ops g_bitbang_ops = {
    .transfer_one  = bb_transfer_one,
    .set_mode      = bb_set_mode,
    .set_speed     = bb_set_speed,
    .set_bit_order = bb_set_bit_order,
    .set_word_size = bb_set_word_size,
};

/**
 * spi_bitbang_register — Register a GPIO-based bit-bang SPI master.
 *
 * @bus_id:     Bus number (0-based, must be < SPI_BUS_MAX)
 * @sclk_port:  I/O port for SCLK
 * @sclk_bit:   Bit position for SCLK
 * @mosi_port:  I/O port for MOSI
 * @mosi_bit:   Bit position for MOSI
 * @miso_port:  I/O port for MISO
 * @miso_bit:   Bit position for MISO
 * @cs_port:    I/O port for CS
 * @cs_bit:     Bit position for CS
 * @speed_hz:   Nominal SPI clock frequency
 *
 * Returns pointer to master on success, NULL on failure.
 */
struct spi_master *spi_bitbang_register(int bus_id,
                                        uint16_t sclk_port, int sclk_bit,
                                        uint16_t mosi_port, int mosi_bit,
                                        uint16_t miso_port, int miso_bit,
                                        uint16_t cs_port, int cs_bit,
                                        uint32_t speed_hz)
{
    if (speed_hz == 0)
        speed_hz = 100000;  /* default 100 kHz */

    struct bitbang_priv *bp = (struct bitbang_priv *)kmalloc(sizeof(struct bitbang_priv));
    if (!bp)
        return NULL;

    memset(bp, 0, sizeof(struct bitbang_priv));
    bp->sclk.port = sclk_port; bp->sclk.bit = sclk_bit;
    bp->mosi.port = mosi_port; bp->mosi.bit = mosi_bit;
    bp->miso.port = miso_port; bp->miso.bit = miso_bit;
    bp->cs.port   = cs_port;   bp->cs.bit   = cs_bit;
    bp->mode      = SPI_MODE_0;
    bp->bit_order = SPI_BIT_ORDER_MSB;
    bp->speed     = speed_hz;

    /* Initialise GPIO pins:
     * CS = HIGH (inactive), SCLK = idle LOW (CPOL=0),
     * MOSI = LOW, MISO = input (drive LOW initially) */
    bb_pin_high(&bp->cs);
    bb_pin_low(&bp->sclk);
    bb_pin_low(&bp->mosi);
    bb_pin_low(&bp->miso);
    bb_delay();

    int ret = spi_master_register(bus_id, &g_bitbang_ops, bp,
                                  SPI_MODE_0, speed_hz);
    if (ret < 0) {
        kfree(bp);
        return NULL;
    }

    kprintf("[SPI] Bitbang master on bus %d: SCLK=0x%x:%d MOSI=0x%x:%d "
            "MISO=0x%x:%d CS=0x%x:%d @%u Hz\n",
            bus_id,
            (unsigned int)sclk_port, sclk_bit,
            (unsigned int)mosi_port, mosi_bit,
            (unsigned int)miso_port, miso_bit,
            (unsigned int)cs_port, cs_bit,
            (unsigned int)speed_hz);

    return spi_get_master(bus_id);
}

/* ── Exported symbols for loadable SPI module drivers ─────────────── */
EXPORT_SYMBOL(spi_master_register);
EXPORT_SYMBOL(spi_master_unregister);
EXPORT_SYMBOL(spi_get_master);
EXPORT_SYMBOL(spi_device_register);
EXPORT_SYMBOL(spi_device_unregister);
EXPORT_SYMBOL(spi_transfer);
EXPORT_SYMBOL(spi_write_then_read);
EXPORT_SYMBOL(spi_bitbang_register);
#include "module.h"
module_init(spi_init);

