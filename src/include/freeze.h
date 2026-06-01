#ifndef FREEZE_H
#define FREEZE_H
#include "types.h"
void freeze_init(void);
int freeze_fs(void);
int thaw_fs(void);
int is_frozen(void);
#endif
