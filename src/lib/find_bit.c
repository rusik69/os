#include "find_bit.h"
#include "string.h"

int test_bit(int nr, const volatile unsigned long *addr) {
    return 1UL & (addr[nr / (8*sizeof(long))] >> (nr % (8*sizeof(long))));
}
void set_bit(int nr, volatile unsigned long *addr) {
    __sync_fetch_and_or(&addr[nr / (8*sizeof(long))], 1UL << (nr % (8*sizeof(long))));
}
void clear_bit(int nr, volatile unsigned long *addr) {
    __sync_fetch_and_and(&addr[nr / (8*sizeof(long))], ~(1UL << (nr % (8*sizeof(long)))));
}

unsigned long find_first_bit(const unsigned long *addr, unsigned long size) {
    for (unsigned long i = 0; i < size; i++)
        if (test_bit((int)i, addr)) return i;
    return size;
}

unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size) {
    for (unsigned long i = 0; i < size; i++)
        if (!test_bit((int)i, addr)) return i;
    return size;
}

static unsigned long find_last_bit(const unsigned long *addr, unsigned long size) {
    if (size == 0) return 0;
    for (unsigned long i = size - 1; ; i--) {
        if (test_bit((int)i, addr)) return i;
        if (i == 0) break;
    }
    return size;
}

static unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
    for (unsigned long i = offset; i < size; i++)
        if (test_bit((int)i, addr)) return i;
    return size;
}

static unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
    for (unsigned long i = offset; i < size; i++)
        if (!test_bit((int)i, addr)) return i;
    return size;
}
