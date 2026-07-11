#ifndef TASKLET_H
#define TASKLET_H
#include "types.h"

struct tasklet_struct {
    void (*func)(unsigned long);
    unsigned long data;
    int state;
};

void tasklet_init(void);
int tasklet_schedule(struct tasklet_struct *t);
int tasklet_kill(struct tasklet_struct *t);
int tasklet_hi_schedule(struct tasklet_struct *t);
int tasklet_enable(struct tasklet_struct *t);
int tasklet_disable(struct tasklet_struct *t);

#endif
