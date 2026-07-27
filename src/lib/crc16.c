#include "types.h"
/**
 * crc16 - Compute CRC-16-CCITT checksum (polynomial 0x1021)
 * @crc: Initial CRC value (typically 0)
 * @buf: Pointer to the input data buffer
 * @len: Length of the input data in bytes
 *
 * Computes a CRC-16-CCITT checksum over the given data buffer using a
 * nibble-based lookup table for efficiency. The CRC-16-CCITT polynomial
 * (0x1021) is used.
 *
 * Context: Any context.
 * Return: The CRC-16 checksum value.
 */
uint16_t crc16(uint16_t crc, const void *buf, uint32_t len) {
    static const uint16_t t[16] = { 0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF };
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < len; i++) { crc = (crc << 4) ^ t[((crc >> 12) ^ (p[i] >> 4)) & 0xF]; crc = (crc << 4) ^ t[((crc >> 12) ^ (p[i] & 0xF)) & 0xF]; }
    return crc;
}

/* ── crc16_byte ─────────────────────────────── */
uint16_t crc16_byte(uint16_t crc, uint8_t byte)
{
    crc = (crc << 4) ^ ((uint16_t[]){ 0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF })[((crc >> 12) ^ (byte >> 4)) & 0xF];
    crc = (crc << 4) ^ ((uint16_t[]){ 0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF })[((crc >> 12) ^ (byte & 0xF)) & 0xF];
    return crc;
}
