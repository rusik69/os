#ifndef IDR_H
#define IDR_H

#include "types.h"

struct idr {
    uint64_t *bitmap;   /* dynamically allocated bitmap */
    int       max;      /* maximum ID value (exclusive) */
    int       nwords;   /* number of words in bitmap */
};

/* Initialize an IDR with max IDs (0..max-1). Returns 0 on success, -1 on failure. */
int idr_init(struct idr *idr, int max);

/* Allocate a new ID. Returns ID (>=0) or -1 if no free IDs. */
int idr_alloc(struct idr *idr);

/* Remove/release an ID back to the pool. */
void idr_remove(struct idr *idr, int id);

/* Check if an ID is allocated. Returns 1 if allocated, 0 if free. */
int idr_find(struct idr *idr, int id);

#endif /* IDR_H */
