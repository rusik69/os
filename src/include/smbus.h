#ifndef SMBUS_H
#define SMBUS_H

#include "types.h"

/* SMBus host controller I/O ports (Intel PCH) */
#define SMBUS_BASE     0xEFD
#define SMBUS_HCTL     0xEFE   /* Host Control */
#define SMBUS_CMD      0xEFB   /* Command */
#define SMBUS_ADDR     0xEFC   /* Slave Address */
#define SMBUS_DATA0    0xEF5   /* Data 0 */
#define SMBUS_DATA1    0xEF4   /* Data 1 */
#define SMBUS_BLOCK    0xEF4   /* Block Data */

/* Host Control bits */
#define SMBUS_HCTL_START    (1 << 0)
#define SMBUS_HCTL_QUICK    (0 << 2)
#define SMBUS_HCTL_BYTE     (1 << 2)
#define SMBUS_HCTL_BYTE_DATA (2 << 2)
#define SMBUS_HCTL_WORD_DATA (3 << 2)
#define SMBUS_HCTL_BLOCK    (5 << 2)
#define SMBUS_HCTL_I2C      (6 << 2)
#define SMBUS_HCTL_KILL     (1 << 1)
#define SMBUS_HCTL_INTR     (1 << 4)
#define SMBUS_HCTL_ERROR    (1 << 5)
#define SMBUS_HCTL_BUSY     (1 << 0)

/* SMBus status */
#define SMBUS_OK        0
#define SMBUS_ERR_TIMEOUT  -1
#define SMBUS_ERR_NODEV    -2

/* SMBus address flags */
#define SMBUS_WRITE 0
#define SMBUS_READ  1

/* API */
int smbus_init(void);
int smbus_is_present(void);
int smbus_read_byte(uint8_t addr, uint8_t reg, uint8_t *val);
int smbus_write_byte(uint8_t addr, uint8_t reg, uint8_t val);
int smbus_read_word(uint8_t addr, uint8_t reg, uint16_t *val);
int smbus_write_word(uint8_t addr, uint8_t reg, uint16_t val);
int smbus_block_read(uint8_t addr, uint8_t cmd, uint8_t *buf, int len);

#endif /* SMBUS_H */
