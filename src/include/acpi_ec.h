#ifndef ACPI_EC_H
#define ACPI_EC_H

#include "types.h"

/* ACPI Embedded Controller (EC) driver functions */

/* Initialize the EC controller.  Detects presence and enables burst mode. */
void ec_init(void);

/* Shut down the EC controller.  Disables burst mode. */
void ec_shutdown(void);

/* Read a single byte from an EC address.  Returns 0 on success. */
int  ec_read(uint8_t addr, uint8_t *val);

/* Write a single byte to an EC address.  Returns 0 on success. */
int  ec_write(uint8_t addr, uint8_t val);

/* Query the EC for a pending event.  Returns the event byte, or -1. */
int  ec_query(void);

/* Check if EC has a SCI event pending.  Returns 1 if pending, 0 otherwise. */
int  ec_sci_pending(void);

/*
 * Read multiple bytes from consecutive EC addresses starting at start_addr.
 * Uses burst mode for efficiency when available.
 * Returns 0 on success, -1 on error.
 */
int  ec_read_burst(uint8_t start_addr, uint8_t *buf, int count);

#endif /* ACPI_EC_H */
