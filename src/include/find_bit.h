#ifndef FIND_BIT_H
#define FIND_BIT_H
unsigned long find_first_bit(const unsigned long *addr, unsigned long size);
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size);
int test_bit(int nr, const volatile unsigned long *addr);
void set_bit(int nr, volatile unsigned long *addr);
void clear_bit(int nr, volatile unsigned long *addr);
#endif
