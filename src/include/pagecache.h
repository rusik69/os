#ifndef PAGECACHE_H
#define PAGECACHE_H
#include "types.h"
void pagecache_init(void);
int pagecache_lookup(uint64_t block, uint8_t *buf);
void pagecache_store(uint64_t block, const uint8_t *data);
#endif
