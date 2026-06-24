#ifndef I3C_H
#define I3C_H

#include "types.h"

/* ── I3C serial bus ───────────────────────────────────────────────── */

/* I3C bus speed modes */
#define I3C_SPEED_SDR         0
#define I3C_SPEED_DDR         1
#define I3C_SPEED_HDR_DDR     2
#define I3C_SPEED_HDR_TSP     3

/* Initialize the I3C subsystem */
void i3c_init(void);

/* Register an I3C controller */
int i3c_controller_register(const char *name, int speed, uint32_t bus_freq_hz);

/* Unregister an I3C controller */
int i3c_controller_unregister(int ctl_id);

/* Perform Dynamic Address Assignment (DAA) */
int i3c_daa(int ctl_id);

/* Add an I3C device to a controller */
int i3c_add_device(int ctl_id, uint8_t static_addr, uint64_t pid,
                   uint8_t bcr, uint8_t dcr);

/* Read data from an I3C device */
ssize_t i3c_read(int ctl_id, uint8_t addr, uint8_t *buf, uint32_t len);

/* Write data to an I3C device */
ssize_t i3c_write(int ctl_id, uint8_t addr, const uint8_t *buf, uint32_t len);

/* Send a CCC (Common Command Code) */
int i3c_send_ccc(int ctl_id, uint8_t ccc, const uint8_t *data, uint32_t len);

/* Handle an In-Band Interrupt (IBI) */
int i3c_handle_ibi(int ctl_id, uint8_t addr,
                   const uint8_t *payload, uint32_t len);

/* Set the I3C bus speed */
int i3c_set_speed(int ctl_id, int speed, uint32_t freq_hz);

/* Get the number of devices on an I3C controller */
int i3c_device_count(int ctl_id);

#endif /* I3C_H */
