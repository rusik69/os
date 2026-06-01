#include "i2c.h"
#include "io.h"
#include "printf.h"

/*
 * I2C bit-banging via GPIO port I/O.
 *
 * We treat each I/O port as a GPIO register where:
 *   bit 0 = SCL (clock line)
 *   bit 1 = SDA (data line)
 *
 * All lines are open-drain; we control them as outputs
 * and read SDA as input when needed via inb().
 *
 * Timing: we use io_wait() and small inline delays for ~100 kHz.
 */

#define SCL_BIT 0
#define SDA_BIT 1
#define SCL_MASK (1U << SCL_BIT)
#define SDA_MASK (1U << SDA_BIT)

static uint16_t g_scl_port = 0;
static uint16_t g_sda_port = 0;
static int g_i2c_initialised = 0;

/* ── Low-level GPIO helpers ────────────────────────────────────────── */

static inline void gpio_scl_high(void) {
    uint8_t v = inb(g_scl_port);
    outb(g_scl_port, v | SCL_MASK);
}

static inline void gpio_scl_low(void) {
    uint8_t v = inb(g_scl_port);
    outb(g_scl_port, v & ~SCL_MASK);
}

static inline void gpio_sda_high(void) {
    uint8_t v = inb(g_sda_port);
    outb(g_sda_port, v | SDA_MASK);
}

static inline void gpio_sda_low(void) {
    uint8_t v = inb(g_sda_port);
    outb(g_sda_port, v & ~SDA_MASK);
}

static inline int gpio_sda_read(void) {
    return (inb(g_sda_port) >> SDA_BIT) & 1;
}

static inline void i2c_delay(void) {
    /* ~5 us delay at QEMU emulation speed */
    io_wait();
    io_wait();
}

/* ── Public API ────────────────────────────────────────────────────── */

void i2c_init(uint16_t scl_port, uint16_t sda_port) {
    g_scl_port = scl_port;
    g_sda_port = sda_port;
    g_i2c_initialised = 1;

    /* Set both lines HIGH (idle bus state) */
    gpio_scl_high();
    gpio_sda_high();
    i2c_delay();

    kprintf("[OK] I2C: bit-bang on SCL=0x%x SDA=0x%x\n",
            (uint32_t)scl_port, (uint32_t)sda_port);
}

void i2c_start(void) {
    if (!g_i2c_initialised) return;

    /* Start condition: SDA transitions HIGH→LOW while SCL is HIGH */
    gpio_sda_high();
    gpio_scl_high();
    i2c_delay();
    gpio_sda_low();
    i2c_delay();
    gpio_scl_low();   /* clock line idle low for data transfer */
    i2c_delay();
}

void i2c_stop(void) {
    if (!g_i2c_initialised) return;

    /* Stop condition: SDA transitions LOW→HIGH while SCL is HIGH */
    gpio_sda_low();
    gpio_scl_high();
    i2c_delay();
    gpio_sda_high();
    i2c_delay();
}

int i2c_write_byte(uint8_t data) {
    if (!g_i2c_initialised) return -1;

    /* Clock out 8 bits, MSB first */
    for (int i = 7; i >= 0; i--) {
        if (data & (1U << i))
            gpio_sda_high();
        else
            gpio_sda_low();
        i2c_delay();

        /* Clock HIGH — slave samples SDA */
        gpio_scl_high();
        i2c_delay();
        gpio_scl_low();
        i2c_delay();
    }

    /* Release SDA for slave ACK */
    gpio_sda_high();
    i2c_delay();

    /* Clock in the ACK bit */
    gpio_scl_high();
    i2c_delay();

    int ack = gpio_sda_read();  /* 0 = ACK, 1 = NAK */

    gpio_scl_low();
    i2c_delay();

    return ack == 0 ? 0 : -1;
}

uint8_t i2c_read_byte(int ack) {
    if (!g_i2c_initialised) return 0xFF;

    uint8_t data = 0;

    /* Release SDA so slave can drive it */
    gpio_sda_high();

    /* Clock in 8 bits, MSB first */
    for (int i = 7; i >= 0; i--) {
        gpio_scl_high();
        i2c_delay();

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
        gpio_sda_high();  /* NAK */
    i2c_delay();

    gpio_scl_high();
    i2c_delay();
    gpio_scl_low();
    i2c_delay();

    /* Release SDA */
    gpio_sda_high();

    return data;
}
