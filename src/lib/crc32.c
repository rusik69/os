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
uint32_t crc32(uint32_t crc, const void *buf, uint32_t len) {
    if (!crc32_initialized) crc32_init_table();
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
uint32_t crc32_no_comp(uint32_t crc, const void *buf, uint32_t len) {
    return crc32(crc, buf, len);
}

/* ── Stub: crc32_le ─────────────────────────────── */
uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t len)
{
    (void)crc;
    (void)data;
    (void)len;
    kprintf("[crc32] crc32_le: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: crc32_be ─────────────────────────────── */
uint32_t crc32_be(uint32_t crc, const uint8_t *data, size_t len)
{
    (void)crc;
    (void)data;
    (void)len;
    kprintf("[crc32] crc32_be: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: crc32c ─────────────────────────────── */
uint32_t crc32c(uint32_t crc, const uint8_t *data, size_t len)
{
    (void)crc;
    (void)data;
    (void)len;
    kprintf("[crc32] crc32c: not yet implemented\n");
    return -ENOSYS;
}
