#include "types.h"
#include "printf.h"
#include "adler32.h"

#define ADLER_MOD ADLER32_MOD

uint32_t adler32(uint32_t adler, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t s1 = adler & 0xFFFF;
    uint32_t s2 = (adler >> 16) & 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        s1 = (s1 + p[i]) % ADLER_MOD;
        s2 = (s2 + s1) % ADLER_MOD;
    }

    return (s2 << 16) | s1;
}

void adler32_init(void)
{
    kprintf("[OK] Adler-32 initialized\n");
}

/* ── Stub: adler32_combine ─────────────────────────────── */
uint32_t adler32_combine(uint32_t crc1, uint32_t crc2, size_t len2)
{
    (void)crc1;
    (void)crc2;
    (void)len2;
    kprintf("[adler32] adler32_combine: not yet implemented\n");
    return -ENOSYS;
}
