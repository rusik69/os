#include "i2c.h"
#include "io.h"
#include "printf.h"

/* I2C bit-banging stub — stores port numbers but does not
   actually drive GPIO lines (no PC parallel port GPIO in QEMU). */

static uint16_t g_scl_port = 0;
static uint16_t g_sda_port = 0;
static int g_i2c_initialised = 0;

void i2c_init(uint16_t scl_port, uint16_t sda_port) {
    g_scl_port = scl_port;
    g_sda_port = sda_port;
    g_i2c_initialised = 1;
    kprintf("[OK] I2C: stub initialised (SCL=0x%x, SDA=0x%x)\n",
            (uint32_t)scl_port, (uint32_t)sda_port);
}

void i2c_start(void) {
    /* Stub: SCL high, SDA falling edge */
    if (!g_i2c_initialised) return;
}

void i2c_stop(void) {
    /* Stub: SCL high, SDA rising edge */
    if (!g_i2c_initialised) return;
}

int i2c_write_byte(uint8_t data) {
    (void)data;
    if (!g_i2c_initialised) return -1;
    /* Stub: assume ACK */
    return 0;
}

uint8_t i2c_read_byte(int ack) {
    (void)ack;
    if (!g_i2c_initialised) return 0xFF;
    /* Stub: return 0xFF */
    return 0xFF;
}
