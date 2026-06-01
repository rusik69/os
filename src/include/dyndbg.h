#ifndef DYNDBG_H
#define DYNDBG_H
#include "types.h"
void dyndbg_init(void);
int dyndbg_register(const char *name);
void dyndbg_enable(const char *name);
void dyndbg_disable(const char *name);
int dyndbg_enabled(const char *name);
#endif
