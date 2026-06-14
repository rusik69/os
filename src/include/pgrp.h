#ifndef PGRP_H
#define PGRP_H
#include "types.h"
#include "process.h"
void pgrp_init(void);
int pgrp_create(struct process *leader);
int pgrp_join(struct process *proc, uint32_t pgid);
void pgrp_set_foreground(uint64_t pgid);
uint64_t pgrp_get_foreground(void);
#endif
