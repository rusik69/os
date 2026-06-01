#ifndef COREDUMP_H
#define COREDUMP_H
#include "types.h"
#include "process.h"
void coredump_init(void);
void coredump_set_enabled(int en);
int coredump_generate(struct process *proc);
#endif
