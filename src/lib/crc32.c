#include "types.h"
#include "printf.h"

/*
 * CRC-32 Lookup Table Generation
 * ===============================
 *
 * CRC-32 uses polynomial division in GF(2) to compute a checksum. The
 * IEEE standard polynomial is 0x04C11DB7, but the table below uses its
 * *reflected* form, 0xEDB88320, because the algorithm processes bytes
 * LSB-first (little-endian convention on the wire and in the x86 CRC
 * instruction).
 *
 * The lookup table is a classic byte-at-a-time optimisation: instead of
 * processing one bit at a time for every byte (8 shifts + XORs per bit =
 * 64 iterations per byte), the table precomputes the CRC contribution of
 * each possible byte value (0-255). The loop then processes one byte per
 * iteration:
 *
 *   index = (current_crc ^ byte) & 0xFF
 *   current_crc = table[index] ^ (current_crc >> 8)
 *
 * Why this works: the CRC of a byte stream can be decomposed because XOR
 * and polynomial division are linear. The table entry for value n is the
 * CRC-32 of the single-byte message [n] (reflected), padded to 32 bits.
 *
 * Table generation for each entry:
 *   1. Start with crc = byte_value (the byte to encode, shifted into the
 *      LSB position because reflection puts the byte in the bottom 8 bits).
 *   2. For each of 8 bits (representing one byte):
 *      a. If the LSB is 1, XOR with the reflected polynomial 0xEDB88320
 *         after shifting right (this is polynomial division by the CRC
 *         generator in reflected form).
 *      b. Shift right by 1.
 *   3. The result is stored in the table indexed by the byte value.
 *
 * The same pattern is repeated for CRC-32C (Castagnoli polynomial
 * 0x82F63B78) and CRC-32BE (non-reflected polynomial 0x04C11DB7).
 */
static uint32_t crc32_table[256];
static int crc32_initialized = 0;

/**
 * crc32_init_table - Precompute the CRC-32 IEEE lookup table
 *
 * Generates a 256-entry table using the bit-at-a-time method.
 * Each entry crc32_table[i] holds the full CRC-32 remainder that
 * results from processing the single byte 'i' (reflected), allowing
 * the byte-at-a-time loop in crc32() to process one byte with a
 * single table lookup, XOR, and shift.
 *
 * Polynomial: 0xEDB88320 (reflected form of IEEE 802.3 0x04C11DB7)
 */
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
/**
 * crc32_be - Compute CRC-32 checksum (big-endian / non-reflected)
 * @crc: Initial CRC value
 * @data: Input data buffer
 * @len: Length in bytes
 *
 * Uses the non-reflected IEEE polynomial 0x04C11DB7 (also known as
 * the POSIX/cksum polynomial, or the original CRC-32 form used by
 * MPEG-2 and Gzip bitstreams). Unlike the reflected (LSB-first)
 * variant, bytes are processed MSB-first: the input byte is placed
 * in the top 8 bits of the CRC register and shifted left.
 *
 * Table generation (non-reflected):
 *   crc_val = byte_value << 24   -- aligned to the high byte
 *   For each of 8 bits:
 *     If MSB (0x80000000) is set, XOR with 0x04C11DB7 after shifting left.
 *     Shift left by 1.
 *
 * The byte-at-a-time loop uses the table indexed by the high byte of
 * the current CRC XOR'd with the next input byte:
 *   index = ((crc >> 24) ^ data[i]) & 0xFF
 *   crc = (crc << 8) ^ table[index]
 */
static uint32_t crc32_be(uint32_t crc, const uint8_t *data, size_t len)
{
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
 * Uses the Castagnoli polynomial (0x82F63B78, reflected form) which
 * is the standard CRC-32C used by Btrfs, ext4, iSCSI, and other
 * storage/file systems. The algorithm is identical to the reflected
 * IEEE CRC-32 but substitutes the Castagnoli polynomial constant.
 *
 * Table generation: same bit-at-a-time reflected method as crc32(),
 * but with polynomial 0x82F63B78 instead of 0xEDB88320.
 *
 * Byte-at-a-time loop:
 *   index = (crc ^ buf[i]) & 0xFF
 *   crc = crc32c_table[index] ^ (crc >> 8)
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
