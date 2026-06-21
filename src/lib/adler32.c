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

/* ── adler32_combine ─────────────────────────────── */
uint32_t adler32_combine(uint32_t crc1, uint32_t crc2, size_t len2)
{
    uint32_t s1a = crc1 & 0xFFFF;
    uint32_t s2a = (crc1 >> 16) & 0xFFFF;
    uint32_t s1b = crc2 & 0xFFFF;
    uint32_t s2b = (crc2 >> 16) & 0xFFFF;

    /* Compute ap = pow(65521, len2) mod 65521 using exponentiation */
    uint64_t ap = 1;
    uint64_t base = ADLER_MOD;
    size_t n = len2;
    while (n > 0) {
        if (n & 1)
            ap = (ap * base) % ADLER_MOD;
        base = (base * base) % ADLER_MOD;
        n >>= 1;
    }

    /* s1_combined = (s1a * ap + s1b) % 65521 */
    uint64_t s1 = ((uint64_t)s1a * ap + s1b) % ADLER_MOD;
    /* s2_combined = (s2a * ap + s2b + len2 * (s1a * ap + 1)) % 65521 */
    uint64_t term = ((uint64_t)s1a * ap + 1) % ADLER_MOD;
    uint64_t s2 = ((uint64_t)s2a * ap + s2b + (len2 % ADLER_MOD) * term) % ADLER_MOD;

    return (uint32_t)((s2 << 16) | s1);
}
