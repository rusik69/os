#include "types.h"
#include "printf.h"
/* CRC32 lookup table (IEEE polynomial 0xEDB88320) */
static uint32_t crc32_table[256];
static int crc32_initialized = 0;
static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
        crc32_table[i] = crc;
    }
    crc32_initialized = 1;
}
/**
 * crc32 - Compute CRC-32 checksum (IEEE polynomial 0xEDB88320)
 * @crc: Initial CRC value (typically 0)
 * @buf: Pointer to the input data buffer
 * @len: Length of the input data in bytes
 *
 * Computes a CRC-32 checksum over the given data buffer using the
 * IEEE polynomial. Uses a 256-entry lookup table initialized on first call.
 * The caller may chain CRC computations by passing the previous return value
 * as @crc for subsequent blocks.
 *
 * Context: Any context. Table initialization on first call is not thread-safe;
 *          call crc32(0, NULL, 0) once at boot from a safe context to pre-init.
 * Return: The CRC-32 checksum (inverted, so final value is the standard CRC).
 */
uint32_t crc32(uint32_t crc, const void *buf, uint32_t len) {
    if (!crc32_initialized) crc32_init_table();
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static uint32_t crc32_no_comp(uint32_t crc, const void *buf, uint32_t len) {
    return crc32(crc, buf, len);
}

/* ── crc32_le ─────────────────────────────── */
static uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t len)
{
    return crc32(crc, data, (uint32_t)len);
}
/* ── crc32_be ─────────────────────────────── */
static uint32_t crc32_be(uint32_t crc, const uint8_t *data, size_t len)
{
    /* CRC32_BE uses bit-reversed polynomial 0x04C11DB7 */
    static uint32_t crc32_be_table[256];
    static int be_initialized = 0;
    if (!be_initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc_val = i << 24;
            for (int j = 0; j < 8; j++)
                crc_val = (crc_val << 1) ^ ((crc_val & 0x80000000) ? 0x04C11DB7 : 0);
            crc32_be_table[i] = crc_val;
        }
        be_initialized = 1;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ crc32_be_table[((crc >> 24) ^ data[i]) & 0xFF];
    return ~crc;
}
/* ── crc32c ─────────────────────────────── */
/**
 * crc32c - Compute CRC-32C checksum (Castagnoli polynomial 0x82F63B78)
 * @crc: Initial CRC value (typically 0 for new checksum)
 * @buf: Pointer to input data
 * @len: Length of input data in bytes
 *
 * Returns: CRC-32C checksum
 *
 * Uses the Castagnoli polynomial (0x82F63B78), which is the standard
 * CRC-32C used by Btrfs and other storage/file systems.
 */
uint32_t crc32c(uint32_t crc, const void *buf, uint32_t len)
{
    /* CRC32C uses polynomial 0x82F63B78 (Castagnoli) */
    static uint32_t crc32c_table[256];
    static int c_initialized = 0;
    if (!c_initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc_val = i;
            for (int j = 0; j < 8; j++)
                crc_val = (crc_val >> 1) ^ (0x82F63B78UL & -(crc_val & 1));
            crc32c_table[i] = crc_val;
        }
        c_initialized = 1;
    }
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++)
        crc = crc32c_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
