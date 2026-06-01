#ifndef PGRP_H
#define PGRP_H
#include "types.h"
#include "process.h"
void pgrp_init(void);
int pgrp_create(struct process *leader);
int pgrp_join(struct process *proc, uint32_t pgid);
#endif
