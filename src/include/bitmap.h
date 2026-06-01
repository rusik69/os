#ifndef BITMAP_H
#define BITMAP_H
void bitmap_zero(unsigned long *dst, int nbits);
void bitmap_set(unsigned long *map, int start, int nr);
void bitmap_clear(unsigned long *map, int start, int nr);
int bitmap_find_next_zero_area(unsigned long *map, int size, int start, int nr);
#endif
