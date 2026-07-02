#ifndef CRC_H
#define CRC_H
#include "types.h"
uint16_t crc16(uint16_t crc, const void *buf, uint32_t len);
uint32_t crc32(uint32_t crc, const void *buf, uint32_t len);
uint32_t crc32c(uint32_t crc, const void *buf, uint32_t len);
#endif
