#ifndef SIGNALFD_H
#define SIGNALFD_H
#include "types.h"
void signalfd_init(void);
int signalfd_create(uint64_t mask);
#endif
