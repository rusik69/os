#ifndef ADLER32_H
#define ADLER32_H

#include "types.h"

#define ADLER32_MOD 65521

/**
 * adler32 - compute Adler-32 checksum
 * @adler: initial checksum value (1 for new checksum)
 * @buf:   input data buffer
 * @len:   length of data in bytes
 * Returns: Adler-32 checksum
 */
uint32_t adler32(uint32_t adler, const void *buf, size_t len);

/* Initialize Adler-32 module */
void adler32_init(void);

#endif /* ADLER32_H */
