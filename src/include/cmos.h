#ifndef CMOS_H
#define CMOS_H

#include "types.h"

/* CMOS address and data ports */
#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

/* CMOS status registers */
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B
#define CMOS_STATUS_C 0x0C
#define CMOS_STATUS_D 0x0D

/* CMOS NVRAM range (14..127 are general-purpose NVRAM bytes) */
#define CMOS_NVRAM_START 14
#define CMOS_NVRAM_END   127
#define CMOS_NVRAM_SIZE  (CMOS_NVRAM_END - CMOS_NVRAM_START + 1)  /* 114 bytes */

/* Read a byte from a CMOS register (reg in 0..0x7F). */
uint8_t cmos_read(uint8_t reg);

/* Write a byte to a CMOS register (reg in 0..0x7F). */
void cmos_write(uint8_t reg, uint8_t val);

/* Read a byte from CMOS NVRAM (offset 0..113 corresponds to reg 14..127). */
uint8_t cmos_nvram_read(uint8_t offset);

/* Write a byte to CMOS NVRAM (offset 0..113 corresponds to reg 14..127). */
void cmos_nvram_write(uint8_t offset, uint8_t val);

/* Initialise CMOS driver (currently a no-op). */
void cmos_init(void);

#endif /* CMOS_H */
