#include "rwsem.h"
#include "scheduler.h"
void rwsem_init(struct rw_semaphore *sem) { sem->count = 0; sem->wait_lock = 0; }
void down_read(struct rw_semaphore *sem) {
    for (;;) {
        if (sem->count >= 0) {
            int old = sem->count;
            if (__sync_bool_compare_and_swap(&sem->count, old, old + 1)) return;
        }
        scheduler_yield();
    }
}
void up_read(struct rw_semaphore *sem) { __sync_fetch_and_sub(&sem->count, 1); }
void down_write(struct rw_semaphore *sem) {
    for (;;) {
        if (sem->count == 0) {
            if (__sync_bool_compare_and_swap(&sem->count, 0, -1)) return;
        }
        scheduler_yield();
    }
}
void up_write(struct rw_semaphore *sem) { __sync_fetch_and_add(&sem->count, 1); }
