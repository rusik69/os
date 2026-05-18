#ifndef MUTEX_H
#define MUTEX_H

/* Kernel mutex — spin+yield, safe on single-CPU preemptive kernel. */

int  mutex_init(void);
void mutex_lock(int id);
void mutex_unlock(int id);
void mutex_destroy(int id);

#endif
