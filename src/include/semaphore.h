#ifndef SEMAPHORE_H
#define SEMAPHORE_H

/* Kernel counting semaphore — spin+yield, safe on single-CPU preemptive kernel. */

int  sem_init(int count);
void sem_wait(int id);
void sem_post(int id);
void sem_destroy(int id);

#endif
