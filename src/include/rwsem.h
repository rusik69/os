#ifndef RWSEM_H
#define RWSEM_H
#include "types.h"
#include "spinlock.h"
struct rw_semaphore { volatile int count; spinlock_t wait_lock; };
void rwsem_init(struct rw_semaphore *sem);
void down_read(struct rw_semaphore *sem);
void up_read(struct rw_semaphore *sem);
void down_write(struct rw_semaphore *sem);
void up_write(struct rw_semaphore *sem);
#endif
