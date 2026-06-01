#ifndef MEMPOOL_H
#define MEMPOOL_H
#include "types.h"
typedef struct { void **elements; int cur_nr, max_nr, elem_size, min_nr; } mempool_t;
mempool_t *mempool_create(int min_nr, int elem_size);
void *mempool_alloc(mempool_t *pool);
void mempool_free(void *element, mempool_t *pool);
void mempool_destroy(mempool_t *pool);
#endif
