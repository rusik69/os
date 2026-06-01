#ifndef CRC64_H
#define CRC64_H

#include "types.h"

#define CRC64_ECMA182_POLY 0xC96C5795D7870F42ULL

/**
 * crc64 - compute CRC64-ECMA-182 checksum
 * @crc: initial CRC value (0 for new checksum)
 * @buf: input data buffer
 * @len: length of data in bytes
 * Returns: CRC64-ECMA-182 value
 */
uint64_t crc64(uint64_t crc, const void *buf, size_t len);

/* Initialize CRC64 module */
void crc64_init(void);

#endif /* CRC64_H */
