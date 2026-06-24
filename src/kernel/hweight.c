#include "hweight.h"
#include "printf.h"
#include "kernel.h"

/**
 * hweight8 - Population count (Hamming weight) for an 8-bit value
 * @x: 8-bit unsigned integer
 *
 * Uses a divide-and-conquer bit-counting algorithm (popcount) to
 * compute the number of set bits.
 *
 * Return: Number of 1-bits in @x (0-8)
 */
unsigned int hweight8(uint8_t x)
{
    x = (x & 0x55) + ((x >> 1) & 0x55);
    x = (x & 0x33) + ((x >> 2) & 0x33);
    x = (x & 0x0F) + ((x >> 4) & 0x0F);
    return x;
}

/**
 * hweight16 - Population count (Hamming weight) for a 16-bit value
 * @x: 16-bit unsigned integer
 *
 * Computes the population count by summing the counts of the upper
 * and lower 8-bit halves via hweight8().
 *
 * Return: Number of 1-bits in @x (0-16)
 */
unsigned int hweight16(uint16_t x)
{
    return hweight8((uint8_t)x) + hweight8((uint8_t)(x >> 8));
}

/**
 * hweight32 - Population count (Hamming weight) for a 32-bit value
 * @x: 32-bit unsigned integer
 *
 * Uses a divide-and-conquer bit-counting algorithm (popcount) to
 * compute the number of set bits in a 32-bit word.
 *
 * Return: Number of 1-bits in @x (0-32)
 */
unsigned int hweight32(uint32_t x)
{
    x = (x & 0x55555555) + ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x & 0x0F0F0F0F) + ((x >> 4) & 0x0F0F0F0F);
    x = (x & 0x00FF00FF) + ((x >> 8) & 0x00FF00FF);
    x = (x & 0x0000FFFF) + ((x >> 16) & 0x0000FFFF);
    return x;
}

/**
 * hweight64 - Population count (Hamming weight) for a 64-bit value
 * @x: 64-bit unsigned integer
 *
 * Computes the population count by summing the counts of the upper
 * and lower 32-bit halves via hweight32().
 *
 * Return: Number of 1-bits in @x (0-64)
 */
unsigned int hweight64(uint64_t x)
{
    return hweight32((uint32_t)x) + hweight32((uint32_t)(x >> 32));
}

void hweight_init(void)
{
    kprintf("[OK] hweight: Hamming weight (popcount) routines initialised\n");
}

