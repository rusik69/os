#include "mempool.h"
#include "heap.h"
#include "string.h"
mempool_t *mempool_create(int min_nr, int elem_size) {
    mempool_t *p = kmalloc(sizeof(mempool_t)); if (!p) return NULL;
    p->min_nr = min_nr;
    /* Guard against overflow: if min_nr > 0x7FFFFFFF/2 we can't safely double */
    if (min_nr > 0x3FFFFFFF || min_nr < 0) {
        p->max_nr = min_nr;
    } else {
        p->max_nr = min_nr * 2;
    }
    p->elem_size = elem_size; p->cur_nr = min_nr;
    p->elements = kmalloc(sizeof(void *) * p->max_nr); if (!p->elements) { kfree(p); return NULL; }
    for (int i = 0; i < min_nr; i++) p->elements[i] = kmalloc(elem_size);
    return p;
}
void *mempool_alloc(mempool_t *pool) { return pool->cur_nr > 0 ? pool->elements[--pool->cur_nr] : kmalloc(pool->elem_size); }
void mempool_free(void *e, mempool_t *p) { if (p->cur_nr < p->max_nr) p->elements[p->cur_nr++] = e; else kfree(e); }
void mempool_destroy(mempool_t *p) { for (int i = 0; i < p->cur_nr; i++) kfree(p->elements[i]); kfree(p->elements); kfree(p); }
