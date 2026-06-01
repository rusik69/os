#ifndef UIO_H
#define UIO_H
#include "types.h"
void uio_init(void);
int uio_register(uint64_t phys_addr, size_t size);
#endif
