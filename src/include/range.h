#ifndef RANGE_H
#define RANGE_H

#include "types.h"

struct range {
    uint64_t start;
    uint64_t end;   /* inclusive */
};

/* Insert [start, end] into an array of ranges (sorted, non-overlapping).
 * Returns the new number of ranges, or -ENOMEM if the array is full
 * (max_size exceeded). */
int range_add(struct range *ranges, int *nr, int max_size,
              uint64_t start, uint64_t end);

/* Remove [start, end] from an array of ranges.
 * Returns the new number of ranges. */
int range_remove(struct range *ranges, int *nr,
                 uint64_t start, uint64_t end);

/* Test whether a value is within any range in the array. */
int range_contains(struct range *ranges, int nr, uint64_t val);

/* Test whether [start, end] overlaps any range in the array. */
int range_overlaps(struct range *ranges, int nr,
                   uint64_t start, uint64_t end);

/* Sort ranges by start address, merge adjacent/overlapping entries. */
void range_sort(struct range *ranges, int *nr);

/* Return the number of ranges. */
static inline int range_count(int nr) { return nr; }

void range_init(void);

#endif /* RANGE_H */
