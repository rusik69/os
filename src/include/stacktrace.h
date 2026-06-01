#ifndef STACKTRACE_H
#define STACKTRACE_H
#include "types.h"
int save_stack_trace(uint64_t *entries, int max_entries);
void print_stack_trace(void);
#endif
