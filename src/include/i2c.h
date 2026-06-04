#ifndef I2C_H
#define I2C_H

#include "types.h"

/* I2C bit-banging via two GPIO ports (SCL = clock, SDA = data).
   Ports are typically PC-style parallel port GPIO or custom. */

/* ── Initialisation ──────────────────────────────────────────────────── */

/* Initialise I2C bus on the given port numbers.
   scl_port: I/O port for clock line.
   sda_port: I/O port for data line. */
void i2c_init(uint16_t scl_port, uint16_t sda_port);

/* ── Master operations ───────────────────────────────────────────────── */

/* Generate I2C start condition (SCL high, SDA falling edge). */
void i2c_start(void);

/* Generate I2C repeat-start condition (re-issue start without stop). */
void i2c_repeated_start(void);

/* Generate I2C stop condition (SCL high, SDA rising edge). */
void i2c_stop(void);

/* Write a single byte to the I2C bus as master.
   Returns 0 if ACK received, -1 if NAK.
   Under multi-master, returns -2 if arbitration was lost. */
int i2c_write_byte(uint8_t data);

/* Read a single byte from the I2C bus as master.
   If ack is non-zero, send ACK; otherwise send NAK. */
uint8_t i2c_read_byte(int ack);

/* ── Multi-master arbitration status ─────────────────────────────────── */

/* Return non-zero if this master lost arbitration in the last transaction.
   Call i2c_arbitration_clear() to reset. */
int i2c_arbitration_lost(void);

/* Clear the arbitration-lost flag. */
void i2c_arbitration_clear(void);

/* ── Slave mode ──────────────────────────────────────────────────────── */

/* Callback type for slave data reception.
   data: the byte received from the master.
   Return 0 to ACK, non-zero to NAK. */
typedef int (*i2c_slave_rx_cb_t)(uint8_t data);

/* Callback type for slave data transmission.
   Returns the byte to send to the master. */
typedef uint8_t (*i2c_slave_tx_cb_t)(void);

/* Callback type for slave STOP condition notification. */
typedef void (*i2c_slave_stop_cb_t)(void);

/* Register this device as an I2C slave with the given 7-bit address.
   When the master addresses this address, the callbacks are invoked.
   Only one slave address can be active at a time.
   Pass NULL for rx/tx to disable slave mode. */
void i2c_slave_register(uint8_t addr_7bit,
                        i2c_slave_rx_cb_t rx_cb,
                        i2c_slave_tx_cb_t tx_cb,
                        i2c_slave_stop_cb_t stop_cb);

/* Unregister the current slave. */
void i2c_slave_unregister(void);

/* Poll the I2C bus for slave transactions.
   Must be called periodically (e.g., from a timer or idle loop)
   to detect start conditions addressed to this slave.
   Returns 0 if no transaction occurred, 1 if a slave transaction
   was handled. */
int i2c_slave_poll(void);

#endif /* I2C_H */
