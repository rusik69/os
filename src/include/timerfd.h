#ifndef TIMERFD_H
#define TIMERFD_H
#include "types.h"
void timerfd_init(void);
int timerfd_create(int clockid);
#endif
