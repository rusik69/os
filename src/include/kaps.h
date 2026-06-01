#ifndef KAPS_H
#define KAPS_H
#include "types.h"
#define CAP_CHOWN 0
#define CAP_DAC_OVERRIDE 1
#define CAP_NET_RAW 13
#define CAP_SYS_ADMIN 21
void caps_init(void);
int caps_capable(uint64_t cap);
int caps_set_effective(uint64_t cap, int set);
int caps_set_bounding(uint64_t cap, int drop);
#endif
