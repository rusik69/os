#include "types.h"
#include "string.h"
#include "printf.h"
#include "crc64.h"

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

/* ── Stub: crc64_le ─────────────────────────────── */
uint64_t crc64_le(uint64_t crc, const uint8_t *data, size_t len)
{
    (void)crc;
    (void)data;
    (void)len;
    kprintf("[crc64] crc64_le: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: crc64_be ─────────────────────────────── */
uint64_t crc64_be(uint64_t crc, const uint8_t *data, size_t len)
{
    (void)crc;
    (void)data;
    (void)len;
    kprintf("[crc64] crc64_be: not yet implemented\n");
    return -ENOSYS;
}
