#include "find_bit.h"
int test_bit(int nr, const volatile unsigned long *addr) { return 1UL & (addr[nr / (8*sizeof(long))] >> (nr % (8*sizeof(long)))); }
void set_bit(int nr, volatile unsigned long *addr) { __sync_fetch_and_or(&addr[nr / (8*sizeof(long))], 1UL << (nr % (8*sizeof(long)))); }
void clear_bit(int nr, volatile unsigned long *addr) { __sync_fetch_and_and(&addr[nr / (8*sizeof(long))], ~(1UL << (nr % (8*sizeof(long))))); }
unsigned long find_first_bit(const unsigned long *addr, unsigned long size) {
    for (unsigned long i = 0; i < size; i++)
        if (test_bit(i, addr)) return i;
    return size;
}
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size) {
    for (unsigned long i = 0; i < size; i++)
        if (!test_bit(i, addr)) return i;
    return size;
}

/* ── Stub: find_first_bit ─────────────────────────────── */
unsigned long find_first_bit(const void *addr, unsigned long size)
{
    (void)addr;
    (void)size;
    kprintf("[find_bit] find_first_bit: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: find_last_bit ─────────────────────────────── */
unsigned long find_last_bit(const void *addr, unsigned long size)
{
    (void)addr;
    (void)size;
    kprintf("[find_bit] find_last_bit: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: find_next_bit ─────────────────────────────── */
unsigned long find_next_bit(const void *addr, unsigned long size, unsigned long offset)
{
    (void)addr;
    (void)size;
    (void)offset;
    kprintf("[find_bit] find_next_bit: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: find_next_zero_bit ─────────────────────────────── */
unsigned long find_next_zero_bit(const void *addr, unsigned long size, unsigned long offset)
{
    (void)addr;
    (void)size;
    (void)offset;
    kprintf("[find_bit] find_next_zero_bit: not yet implemented\n");
    return -ENOSYS;
}
