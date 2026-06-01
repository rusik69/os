#ifndef TRACE_H
#define TRACE_H
#include "types.h"
void trace_init(void);
void trace_write(const char *msg);
void trace_dump(void);
#endif
