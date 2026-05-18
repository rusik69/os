#ifndef SHM_H
#define SHM_H

#include "types.h"

void     shm_init(void);
int      shm_get(int key);         /* get/create segment by key → id or -1 */
uint64_t shm_at(int id);           /* map into current process → virt addr or 0 */
int      shm_dt(int id);           /* decrement ref count */
int      shm_free(int id);         /* free physical frame */

#endif
