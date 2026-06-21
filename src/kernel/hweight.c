#include "hweight.h"
#include "printf.h"
#include "kernel.h"

/* hweight8: population count for 8-bit */
unsigned int hweight8(uint8_t x)
{
    x = (x & 0x55) + ((x >> 1) & 0x55);
    x = (x & 0x33) + ((x >> 2) & 0x33);
    x = (x & 0x0F) + ((x >> 4) & 0x0F);
    return x;
}

/* hweight16: population count for 16-bit */
unsigned int hweight16(uint16_t x)
{
    return hweight8((uint8_t)x) + hweight8((uint8_t)(x >> 8));
}

/* hweight32: population count for 32-bit */
unsigned int hweight32(uint32_t x)
{
    x = (x & 0x55555555) + ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x & 0x0F0F0F0F) + ((x >> 4) & 0x0F0F0F0F);
    x = (x & 0x00FF00FF) + ((x >> 8) & 0x00FF00FF);
    x = (x & 0x0000FFFF) + ((x >> 16) & 0x0000FFFF);
    return x;
}

/* hweight64: population count for 64-bit */
unsigned int hweight64(uint64_t x)
{
    return hweight32((uint32_t)x) + hweight32((uint32_t)(x >> 32));
}

void hweight_init(void)
{
    kprintf("[OK] hweight: Hamming weight (popcount) routines initialised\n");
}

/* ── Stub: hweight8 ─────────────────────────────── */
unsigned int hweight8(uint8_t w)
{
    (void)w;
    kprintf("[hweight] hweight8: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hweight16 ─────────────────────────────── */
unsigned int hweight16(uint16_t w)
{
    (void)w;
    kprintf("[hweight] hweight16: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hweight32 ─────────────────────────────── */
unsigned int hweight32(uint32_t w)
{
    (void)w;
    kprintf("[hweight] hweight32: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hweight64 ─────────────────────────────── */
unsigned int hweight64(uint64_t w)
{
    (void)w;
    kprintf("[hweight] hweight64: not yet implemented\n");
    return -ENOSYS;
}
