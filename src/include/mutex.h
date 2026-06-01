#ifndef MUTEX_H
#define MUTEX_H

#include "types.h"

/* Kernel mutex — spin+yield, safe on single-CPU preemptive kernel. */

int  mutex_init(void);
void mutex_lock(int id);
void mutex_unlock(int id);
void mutex_destroy(int id);

/* Priority Inheritance tracking — boost array indexed by PID */
#define MUTEX_MAX_PI_BOOST 256
extern uint8_t mutex_boost[MUTEX_MAX_PI_BOOST];

#endif
