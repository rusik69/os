#ifndef MSEAL_H
#define MSEAL_H

#include "types.h"

#define MSEAL_SEAL_COUNT 64

struct mseal_range {
    uint64_t virt_start;
    uint64_t virt_end;   /* exclusive end */
    int used;
};

/* Seal the given virtual address range.  Returns 0 on success, -errno on error. */
int mseal(uint64_t addr, uint64_t len, int flags);

/* Check whether the range [addr, addr+len) is fully sealed.  Returns 0 if sealed, -errno if not. */
int mseal_check(uint64_t addr, uint64_t len);

/* Return non-zero if a specific address falls inside any sealed range. */
int mseal_is_sealed(uint64_t addr);

/* Initialise the mseal subsystem. */
void mseal_init(void);

#endif /* MSEAL_H */
