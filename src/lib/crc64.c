#include "crc64.h"
#include "string.h"
#include "printf.h"
#include "types.h"

/* CRC64-ECMA-182 lookup table */
static uint64_t crc64_table[256];
static int crc64_initialized = 0;

static void crc64_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint64_t crc = (uint64_t)i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ CRC64_ECMA182_POLY;
            else
                crc >>= 1;
        }
        crc64_table[i] = crc;
    }
    crc64_initialized = 1;
}

/**
 * crc64 - Compute CRC-64-ECMA-182 checksum
 * @crc: Initial CRC value (typically 0)
 * @buf: Pointer to the input data buffer
 * @len: Length of the input data in bytes
 *
 * Computes a CRC-64 checksum over the given data buffer using the
 * ECMA-182 polynomial. Uses a 256-entry lookup table initialized on
 * first call. The caller may chain CRC computations by passing the
 * previous return value as @crc for subsequent blocks.
 *
 * Context: Any context. Table initialization on first call is not thread-safe;
 *          call crc64(0, NULL, 0) once at boot from a safe context to pre-init.
 * Return: The CRC-64-ECMA-182 checksum.
 */
uint64_t crc64(uint64_t crc, const void *buf, size_t len)
{
    if (!crc64_initialized)
        crc64_init_table();

    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = crc64_table[(uint8_t)(crc ^ p[i])] ^ (crc >> 8);
    return ~crc;
}

void crc64_init(void)
{
    kprintf("[OK] CRC64-ECMA-182 initialized\n");
}

/* ── crc64_le ─────────────────────────────── */
static uint64_t crc64_le(uint64_t crc, const uint8_t *data, size_t len)
{
    return crc64(crc, data, len);
}
/* ── crc64_be ─────────────────────────────── */
static uint64_t crc64_be(uint64_t crc, const uint8_t *data, size_t len)
{
    static uint64_t crc64_be_table[256];
    static int be_initialized = 0;
    if (!be_initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint64_t crc_val = (uint64_t)i << 56;
            for (int j = 0; j < 8; j++)
                crc_val = (crc_val << 1) ^ ((crc_val & (1ULL << 63)) ? CRC64_ECMA182_POLY : 0);
            crc64_be_table[i] = crc_val;
        }
        be_initialized = 1;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ crc64_be_table[((crc >> 56) ^ data[i]) & 0xFF];
    return ~crc;
}
