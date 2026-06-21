#include "i2c.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/*
 * I2C bit-banging via GPIO port I/O with multi-master arbitration and
 * slave-mode support (Item 198).
 *
 * We treat each I/O port as a GPIO register where:
 *   bit 0 = SCL (clock line)
 *   bit 1 = SDA (data line)
 *
 * All lines are open-drain; we control them as outputs
 * and read SDA as input when needed via inb().
 *
 * Multi-master arbitration:
 *   Before driving SDA low, we release SDA and check if it actually
 *   went low. If another master is driving it high and we want low,
 *   we lose arbitration and must release the bus.
 *
 * Slave mode:
 *   By periodically polling (i2c_slave_poll()), we detect start
 *   conditions on the bus, check if the address matches ours,
 *   and handle data transfer via callbacks.
 */

/* ── Bit masks ──────────────────────────────────────────────────────── */

#define SCL_BIT 0
#define SDA_BIT 1
#define SCL_MASK (1U << SCL_BIT)
#define SDA_MASK (1U << SDA_BIT)

/* ── Static state ───────────────────────────────────────────────────── */

static uint16_t g_scl_port = 0;
static uint16_t g_sda_port = 0;
static int g_i2c_initialised = 0;

/* Multi-master arbitration state */
static volatile int g_arbitration_lost = 0;

/* Slave mode state */
static struct {
    int                active;
    uint8_t            addr;           /* 7-bit slave address (upper 7 bits) */
    i2c_slave_rx_cb_t  rx_cb;         /* data reception callback */
    i2c_slave_tx_cb_t  tx_cb;         /* data transmission callback */
    i2c_slave_stop_cb_t stop_cb;       /* stop condition notification */
    uint8_t            last_tx_byte;   /* pre-loaded byte for transmission */
    int                tx_loaded;      /* 1 = tx byte is ready */
    int                in_transaction; /* 1 = currently in a slave transaction */
} g_slave;

/* ── Timing helper ──────────────────────────────────────────────────── */

/* ~5 us delay at QEMU emulation speed; multi-master needs tighter timing */
static inline void i2c_delay(void) {
    io_wait();
    io_wait();
}

/* ── Low-level GPIO helpers ─────────────────────────────────────────── */

static inline void gpio_scl_high(void) {
    outb(g_scl_port, (uint8_t)(inb(g_scl_port) | SCL_MASK));
}

static inline void gpio_scl_low(void) {
    outb(g_scl_port, (uint8_t)(inb(g_scl_port) & ~SCL_MASK));
}

static inline void gpio_sda_high(void) {
    outb(g_sda_port, (uint8_t)(inb(g_sda_port) | SDA_MASK));
}

static inline void gpio_sda_low(void) {
    outb(g_sda_port, (uint8_t)(inb(g_sda_port) & ~SDA_MASK));
}

static inline void gpio_sda_release(void) {
    gpio_sda_high(); /* open-drain: high = released (pull-up keeps it high) */
}

static inline int gpio_sda_read(void) {
    return (inb(g_sda_port) >> SDA_BIT) & 1;
}

static inline int gpio_scl_read(void) {
    return (inb(g_scl_port) >> SCL_BIT) & 1;
}

/* ── Arbitration helpers (multi-master) ─────────────────────────────── */

/*
 * Attempt to drive SDA low.  If another master is driving it high,
 * the line won't go low and we lose arbitration.
 * Returns 0 on success, -1 if arbitration lost.
 */
static inline int arb_sda_low(void) {
    /* Release SDA first so we can sample the actual line value */
    gpio_sda_release();
    i2c_delay();
    if (gpio_sda_read() == 0) {
        /* Line is already low — another master is driving it */
        g_arbitration_lost = 1;
        return -1;
    }
    /* Drive SDA low */
    gpio_sda_low();
    i2c_delay();
    /* Verify we still own the line */
    if (gpio_sda_read() != 0) {
        g_arbitration_lost = 1;
        gpio_sda_release();
        return -1;
    }
    return 0;
}

/*
 * Clock a single SCL cycle: pull SCL low, wait, release, wait.
 * Returns 0 on success, -1 if another master stretches the clock
 * (SCL held low beyond timeout).
 */
static inline int clock_cycle(void) {
    gpio_scl_low();
    i2c_delay();

    /* Release SCL — allow clock stretching */
    gpio_scl_high();
    i2c_delay();

    /* Check for clock stretch: if SCL doesn't go high after release,
     * another (slower) master or a slave is stretching the clock.
     * Wait with a simple timeout. */
    int timeout = 1000;
    while (gpio_scl_read() == 0 && timeout > 0) {
        i2c_delay();
        timeout--;
    }
    if (timeout == 0) {
        /* Clock line stuck low — bus error */
        return -1;
    }
    return 0;
}

/* ── Initialisation ─────────────────────────────────────────────────── */

void i2c_init(uint16_t scl_port, uint16_t sda_port) {
    g_scl_port = scl_port;
    g_sda_port = sda_port;
    g_i2c_initialised = 1;
    g_arbitration_lost = 0;

    /* Reset slave state */
    memset(&g_slave, 0, sizeof(g_slave));

    /* Set both lines HIGH (idle bus state) — release for multi-master */
    gpio_sda_release();
    gpio_scl_high();
    i2c_delay();

    kprintf("[OK] I2C: bit-bang on SCL=0x%x SDA=0x%x (multi-master, slave-mode ready)\n",
            (uint32_t)scl_port, (uint32_t)sda_port);
}

/* ── Multi-master arbitration status ────────────────────────────────── */

int i2c_arbitration_lost(void) {
    return g_arbitration_lost;
}

void i2c_arbitration_clear(void) {
    g_arbitration_lost = 0;
}

/* ── Master operations ──────────────────────────────────────────────── */

void i2c_start(void) {
    if (!g_i2c_initialised) return;
    g_arbitration_lost = 0;

    /* Start condition: SDA transitions HIGH→LOW while SCL is HIGH.
     * For multi-master arbitration, we first make sure we can pull SDA low. */
    gpio_sda_release();
    gpio_scl_high();
    i2c_delay();

    /* Check that SCL actually went high (not stretched) */
    if (gpio_scl_read() == 0) {
        g_arbitration_lost = 1;
        return;
    }

    /* Try to pull SDA low — if another master already has it low, we fail */
    if (arb_sda_low() < 0)
        return;  /* arbitration lost */

    i2c_delay();
    gpio_scl_low();   /* clock line idle low for data transfer */
    i2c_delay();
}

void i2c_repeated_start(void) {
    if (!g_i2c_initialised) return;

    /* Repeated start: SCL low, then release SDA, then SCL high, then SDA low.
     * No stop condition issued — keeps bus ownership. */
    gpio_scl_low();
    i2c_delay();

    gpio_sda_release();
    i2c_delay();

    gpio_scl_high();
    i2c_delay();

    if (gpio_scl_read() == 0) {
        g_arbitration_lost = 1;
        return;
    }

    if (arb_sda_low() < 0)
        return;

    i2c_delay();
    gpio_scl_low();
    i2c_delay();
}

void i2c_stop(void) {
    if (!g_i2c_initialised) return;

    /* Stop condition: SDA transitions LOW→HIGH while SCL is HIGH */
    gpio_sda_low();
    i2c_delay();

    gpio_scl_high();
    i2c_delay();

    if (gpio_scl_read() == 0) {
        /* Clock stretched on stop — rare, but handle */
        g_arbitration_lost = 1;
    }

    gpio_sda_release();
    i2c_delay();
}

int i2c_write_byte(uint8_t data) {
    if (!g_i2c_initialised) return -1;
    if (g_arbitration_lost) return -2;

    /* Clock out 8 bits, MSB first, with arbitration checking */
    for (int i = 7; i >= 0; i--) {
        int want_low = !(data & (1U << i));

        if (want_low) {
            /* Drive SDA low with arbitration check */
            gpio_sda_low();
            i2c_delay();
            /* Verify we still own SDA (multi-master check) */
            if (gpio_sda_read() != 0) {
                g_arbitration_lost = 1;
                gpio_sda_release();
                gpio_scl_low();  /* release SCL too */
                return -2;
            }
        } else {
            /* Release SDA for the bit (it stays high via pull-up) */
            gpio_sda_release();
            i2c_delay();
        }

        /* Clock HIGH — slave samples SDA */
        int ret = clock_cycle();
        if (ret < 0) {
            gpio_sda_release();
            return -1;  /* clock stretch timeout */
        }
        gpio_scl_low();
        i2c_delay();
    }

    /* Release SDA for slave ACK */
    gpio_sda_release();
    i2c_delay();

    /* Clock in the ACK bit (with clock stretch handling) */
    gpio_scl_high();
    i2c_delay();
    if (gpio_scl_read() == 0) {
        /* Clock stretch during ACK */
        int timeout = 1000;
        while (gpio_scl_read() == 0 && timeout > 0) {
            i2c_delay();
            timeout--;
        }
        if (timeout == 0) return -1;
    }

    int ack = gpio_sda_read();  /* 0 = ACK, 1 = NAK */
    gpio_scl_low();
    i2c_delay();

    return ack == 0 ? 0 : -1;
}

uint8_t i2c_read_byte(int ack) {
    if (!g_i2c_initialised) return 0xFF;

    uint8_t data = 0;

    /* Release SDA so slave can drive it */
    gpio_sda_release();
    i2c_delay();

    /* Clock in 8 bits, MSB first */
    for (int i = 7; i >= 0; i--) {
        int ret = clock_cycle();
        if (ret < 0) return 0xFF;  /* clock stretch timeout */

        data <<= 1;
        if (gpio_sda_read())
            data |= 1;

        gpio_scl_low();
        i2c_delay();
    }

    /* Send ACK or NAK */
    if (ack)
        gpio_sda_low();   /* ACK */
    else
        gpio_sda_release();  /* NAK */
    i2c_delay();

    gpio_scl_high();
    i2c_delay();
    gpio_scl_low();

    /* Verify arbitration on ACK bit for multi-master */
    if (ack && gpio_sda_read() != 0) {
        /* We tried to send ACK but line is high — arbitration lost */
        g_arbitration_lost = 1;
    }

    /* Release SDA */
    gpio_sda_release();

    return data;
}

/* ── Convenience: combined master write with stop ───────────────────── */

int i2c_master_write(uint8_t dev_addr, const uint8_t *data, int len, int send_stop) {
    if (!g_i2c_initialised || !data || len <= 0) return -1;

    i2c_start();
    if (g_arbitration_lost) return -2;

    /* Send address (7-bit left-shifted, write = 0) */
    if (i2c_write_byte((uint8_t)(dev_addr << 1)) < 0) {
        if (send_stop) i2c_stop();
        return -1;
    }

    /* Send data bytes */
    for (int i = 0; i < len; i++) {
        if (i2c_write_byte(data[i]) < 0) {
            if (send_stop) i2c_stop();
            return -1;
        }
    }

    if (send_stop)
        i2c_stop();

    return 0;
}

int i2c_master_read(uint8_t dev_addr, uint8_t *buf, int len, int send_stop) {
    if (!g_i2c_initialised || !buf || len <= 0) return -1;

    /* Repeated start followed by address + read bit */
    i2c_repeated_start();
    if (g_arbitration_lost) return -2;

    /* Send address (7-bit left-shifted, read = 1) */
    if (i2c_write_byte((uint8_t)(dev_addr << 1) | 1) < 0) {
        if (send_stop) i2c_stop();
        return -1;
    }

    /* Read data bytes — ACK all but the last */
    for (int i = 0; i < len; i++) {
        buf[i] = i2c_read_byte(i < len - 1 ? 1 : 0);
    }

    if (send_stop)
        i2c_stop();

    return len;
}

/* ── Slave mode ─────────────────────────────────────────────────────── */

void i2c_slave_register(uint8_t addr_7bit,
                        i2c_slave_rx_cb_t rx_cb,
                        i2c_slave_tx_cb_t tx_cb,
                        i2c_slave_stop_cb_t stop_cb) {
    g_slave.addr        = addr_7bit & 0x7F;  /* mask to 7 bits */
    g_slave.rx_cb       = rx_cb;
    g_slave.tx_cb       = tx_cb;
    g_slave.stop_cb     = stop_cb;
    g_slave.active      = (rx_cb != NULL || tx_cb != NULL) ? 1 : 0;
    g_slave.tx_loaded   = 0;
    g_slave.in_transaction = 0;

    if (g_slave.active) {
        kprintf("[I2C] Slave registered at address 0x%02X (7-bit)\n",
                (unsigned)addr_7bit);
    }
}

void i2c_slave_unregister(void) {
    memset(&g_slave, 0, sizeof(g_slave));
    kprintf("[I2C] Slave unregistered\n");
}

/*
 * Wait for SCL to be released (clock stretch / start condition).
 * Returns 0 on success, -1 if SCL never goes high (bus lock).
 */
static int wait_scl_high(void) {
    int timeout = 5000;
    while (gpio_scl_read() == 0 && timeout > 0) {
        i2c_delay();
        timeout--;
    }
    return (timeout > 0) ? 0 : -1;
}

/*
 * Detect an I2C start condition: SDA falling while SCL is high.
 * Returns 1 if detected, 0 if not.
 * We sample SCL high, then sample SDA — if SDA == 0, a start occurred.
 */
static int detect_start(void) {
    if (!g_i2c_initialised) return 0;

    /* SCL must be high / released */
    if (wait_scl_high() < 0) return 0;

    /* Sample SDA — if it's low, a start condition might be in progress */
    if (gpio_sda_read() == 0) {
        /* Wait a short time and check SCL is still high */
        i2c_delay();
        if (gpio_scl_read() != 0) {
            return 1;  /* start condition detected */
        }
    }
    return 0;
}

/*
 * Receive a single byte as slave (clock in 8 bits from master).
 * Returns the received byte.
 */
static uint8_t slave_receive_byte(void) {
    uint8_t data = 0;

    for (int i = 7; i >= 0; i--) {
        /* Wait for SCL to go high (master releases clock) */
        if (wait_scl_high() < 0) return 0xFF;

        /* Sample SDA */
        data <<= 1;
        if (gpio_sda_read())
            data |= 1;

        /* Wait for SCL to go low again */
        int timeout = 1000;
        while (gpio_scl_read() != 0 && timeout > 0) {
            i2c_delay();
            timeout--;
        }
        if (timeout == 0) return 0xFF;
    }

    return data;
}

/*
 * Send a single byte as slave (clock out 8 bits to master).
 */
static void slave_send_byte(uint8_t data) {
    for (int i = 7; i >= 0; i--) {
        if (data & (1U << i))
            gpio_sda_release();  /* high */
        else
            gpio_sda_low();     /* low */
        i2c_delay();

        /* Release SCL to let master clock */
        gpio_scl_high();
        i2c_delay();

        /* Wait for master to pull SCL low */
        int timeout = 1000;
        while (gpio_scl_read() != 0 && timeout > 0) {
            i2c_delay();
            timeout--;
        }
        if (timeout == 0) {
            /* Master released bus — abort */
            gpio_sda_release();
            return;
        }
    }

    /* Release SDA for master ACK */
    gpio_sda_release();
    i2c_delay();

    /* Let master clock ACK */
    gpio_scl_high();
    i2c_delay();
    if (wait_scl_high() < 0) {
        gpio_sda_release();
        return;
    }
    gpio_scl_low();
}

int i2c_slave_poll(void) {
    if (!g_i2c_initialised || !g_slave.active)
        return 0;

    /* Detect start condition */
    if (!detect_start())
        return 0;

    g_slave.in_transaction = 1;

    /* We're now in a start condition.  Release SDA and wait for
     * the master to clock out the address byte.  As a slave, we
     * must not drive SCL or SDA unless responding. */
    gpio_sda_release();
    i2c_delay();

    /* Receive the address byte (7-bit addr + R/W bit) */
    uint8_t addr_byte = slave_receive_byte();
    if (addr_byte == 0xFF) {
        g_slave.in_transaction = 0;
        return 0;  /* bus error or timeout */
    }

    uint8_t target_addr = addr_byte >> 1;       /* upper 7 bits */
    int is_read = addr_byte & 1;                /* LSB = R/W */

    /* Check if the address matches ours */
    if (target_addr != g_slave.addr) {
        /* Not for us — just release the bus */
        gpio_sda_release();
        g_slave.in_transaction = 0;
        return 0;
    }

    /* Our address!  Send ACK: pull SDA low during the ACK clock cycle */
    gpio_sda_low();
    i2c_delay();
    gpio_scl_high();
    i2c_delay();
    if (wait_scl_high() < 0) {
        gpio_sda_release();
        g_slave.in_transaction = 0;
        return 0;
    }
    gpio_scl_low();
    gpio_sda_release();
    i2c_delay();

    if (is_read) {
        /* Master is reading from us — we transmit data */
        if (g_slave.tx_cb) {
            /* Pre-load first byte */
            uint8_t tx_byte = g_slave.tx_cb();
            slave_send_byte(tx_byte);

            /* Continue sending while master ACKs */
            for (int i = 0; i < 255; i++) {
                /* Check for stop / repeated start by monitoring SDA
                 * during SCL high periods.  If SDA rises while SCL high,
                 * that's a stop; if SDA falls while SCL high, that's
                 * a repeated start. */
                gpio_scl_high();
                i2c_delay();
                if (gpio_scl_read() == 0) continue;  /* clock stretch */

                int sda_val = gpio_sda_read();
                if (sda_val == 1) {
                    /* Stop condition — SDA rising while SCL high */
                    gpio_scl_low();
                    break;
                }
                /* Check if it's a repeated start (SDA low while SCL high) */
                if (sda_val == 0) {
                    /* Could be a repeated start — address will come next */
                    gpio_scl_low();
                    break;
                }
                gpio_scl_low();
                i2c_delay();

                /* Master should ACK each byte; if it NAKs, stop sending */
                gpio_scl_high();
                i2c_delay();
                int master_ack = gpio_sda_read();
                gpio_scl_low();

                if (master_ack != 0) {
                    /* NAK — master is done reading */
                    break;
                }

                /* Send next byte */
                tx_byte = g_slave.tx_cb();
                slave_send_byte(tx_byte);
            }
        }
    } else {
        /* Master is writing to us — we receive data */
        if (g_slave.rx_cb) {
            for (int i = 0; i < 255; i++) {
                /* Receive a byte */
                uint8_t rx_byte = slave_receive_byte();
                if (rx_byte == 0xFF) break;  /* bus error */

                /* Call the receive callback */
                int nak = g_slave.rx_cb(rx_byte);

                /* Send ACK/NAK */
                if (nak) {
                    gpio_sda_release();  /* NAK */
                } else {
                    gpio_sda_low();     /* ACK */
                }
                i2c_delay();
                gpio_scl_high();
                i2c_delay();
                if (wait_scl_high() < 0) break;
                gpio_scl_low();
                gpio_sda_release();
                i2c_delay();

                /* Check for stop condition */
                gpio_scl_high();
                i2c_delay();
                int sda_val = gpio_sda_read();
                if (sda_val == 1) {
                    /* Stop condition */
                    gpio_scl_low();
                    break;
                }
                gpio_scl_low();
                i2c_delay();
            }
        }
    }

    /* Notify stop callback */
    if (g_slave.stop_cb)
        g_slave.stop_cb();

    /* Release bus */
    gpio_sda_release();
    gpio_scl_high();
    i2c_delay();

    g_slave.in_transaction = 0;
    return 1;
}

/* ── Stub: i2c_read ─────────────────────────────── */
int i2c_read(int addr, void *buf, size_t count)
{
    (void)addr;
    (void)buf;
    (void)count;
    kprintf("[i2c] i2c_read: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: i2c_write ─────────────────────────────── */
int i2c_write(int addr, const void *buf, size_t count)
{
    (void)addr;
    (void)buf;
    (void)count;
    kprintf("[i2c] i2c_write: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: i2c_transfer ─────────────────────────────── */
int i2c_transfer(void *msgs, int num)
{
    (void)msgs;
    (void)num;
    kprintf("[i2c] i2c_transfer: not yet implemented\n");
    return -ENOSYS;
}
