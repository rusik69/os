#ifndef MADVISE_EXT_H
#define MADVISE_EXT_H

#include "types.h"

/* Extended madvise operations */
int madvise_dontneed(uint64_t addr, uint64_t len);
int madvise_willneed(uint64_t addr, uint64_t len);
int madvise_cold(uint64_t addr, uint64_t len);
int madvise_pageout(uint64_t addr, uint64_t len);
int madvise_free(uint64_t addr, uint64_t len);
int madvise_mergeable(uint64_t addr, uint64_t len);

/* Initialise the madvise_ext subsystem. */
void madvise_ext_init(void);

#endif /* MADVISE_EXT_H */
