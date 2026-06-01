#ifndef I2C_H
#define I2C_H

#include "types.h"

/* I2C bit-banging via two GPIO ports (SCL = clock, SDA = data).
   Ports are typically PC-style parallel port GPIO or custom.
   For stub purposes, scl_port and sda_port are stored but unused. */

/* Initialise I2C bus on the given port numbers.
   scl_port: I/O port for clock line.
   sda_port: I/O port for data line. */
void i2c_init(uint16_t scl_port, uint16_t sda_port);

/* Generate I2C start condition (SCL high, SDA falling edge). */
void i2c_start(void);

/* Generate I2C stop condition (SCL high, SDA rising edge). */
void i2c_stop(void);

/* Write a single byte to the I2C bus.
   Returns 0 if ACK received, -1 if NAK. */
int i2c_write_byte(uint8_t data);

/* Read a single byte from the I2C bus.
   If ack is non-zero, send ACK; otherwise send NAK. */
uint8_t i2c_read_byte(int ack);

#endif /* I2C_H */
