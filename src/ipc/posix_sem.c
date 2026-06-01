#define KERNEL_INTERNAL
#include "types.h"
#include "tmpfs.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"
#include "mutex.h"
#include "scheduler.h"
#include "process.h"

/* ── POSIX named semaphores ──────────────────────────────────────────── */

#define SEM_MAX 32
#define SEM_NAME_MAX 64

struct posix_sem {
    char name[SEM_NAME_MAX];
    int count;
    int in_use;
    uint32_t owner_pid;
    int refcount;
    mutex_t lock;
};

static struct posix_sem sem_table[SEM_MAX];
static int sem_initialized = 0;
static mutex_t sem_global_lock;

/* Initialize the named semaphore subsystem */
void posix_sem_init(void) {
    if (sem_initialized) return;
    memset(sem_table, 0, sizeof(sem_table));
    mutex_init(&sem_global_lock);
    sem_initialized = 1;
    kprintf("[OK] POSIX named semaphores initialized\n");
}

/* Open a named semaphore (create if not exists) */
int posix_sem_open(const char *name, int oflag, int mode, unsigned int value) {
    (void)mode;
    if (!name) return -EINVAL;
    
    mutex_lock(&sem_global_lock);
    
    /* Check if semaphore already exists */
    for (int i = 0; i < SEM_MAX; i++) {
        if (sem_table[i].in_use && strcmp(sem_table[i].name, name) == 0) {
            sem_table[i].refcount++;
            mutex_unlock(&sem_global_lock);
            return i; /* returns sem_id */
        }
    }
    
    if (!(oflag & 1)) { /* O_CREAT */
        mutex_unlock(&sem_global_lock);
        return -ENOENT;
    }
    
    /* Create new semaphore */
    for (int i = 0; i < SEM_MAX; i++) {
        if (!sem_table[i].in_use) {
            strncpy(sem_table[i].name, name, SEM_NAME_MAX - 1);
            sem_table[i].name[SEM_NAME_MAX - 1] = '\0';
            sem_table[i].count = (int)value;
            sem_table[i].in_use = 1;
            sem_table[i].refcount = 1;
            mutex_init(&sem_table[i].lock);
            struct process *cur = process_get_current();
            sem_table[i].owner_pid = cur ? cur->pid : 0;
            
            mutex_unlock(&sem_global_lock);
            return i;
        }
    }
    
    mutex_unlock(&sem_global_lock);
    return -ENFILE;
}

/* Close a named semaphore */
int posix_sem_close(int sem_id) {
    if (sem_id < 0 || sem_id >= SEM_MAX) return -EINVAL;
    
    mutex_lock(&sem_global_lock);
    if (!sem_table[sem_id].in_use) {
        mutex_unlock(&sem_global_lock);
        return -EINVAL;
    }
    
    sem_table[sem_id].refcount--;
    if (sem_table[sem_id].refcount <= 0) {
        memset(&sem_table[sem_id], 0, sizeof(struct posix_sem));
    }
    mutex_unlock(&sem_global_lock);
    return 0;
}

/* Unlink a named semaphore (mark for deletion on last close) */
int posix_sem_unlink(const char *name) {
    if (!name) return -EINVAL;
    
    mutex_lock(&sem_global_lock);
    for (int i = 0; i < SEM_MAX; i++) {
        if (sem_table[i].in_use && strcmp(sem_table[i].name, name) == 0) {
            sem_table[i].refcount--;
            if (sem_table[i].refcount <= 0) {
                memset(&sem_table[i], 0, sizeof(struct posix_sem));
            }
            mutex_unlock(&sem_global_lock);
            return 0;
        }
    }
    mutex_unlock(&sem_global_lock);
    return -ENOENT;
}

/* Wait (decrement) semaphore */
int posix_sem_wait(int sem_id) {
    if (sem_id < 0 || sem_id >= SEM_MAX) return -EINVAL;
    if (!sem_table[sem_id].in_use) return -EINVAL;
    
    struct posix_sem *sem = &sem_table[sem_id];
    /* Spin-wait for simplicity */
    while (1) {
        mutex_lock(&sem->lock);
        if (sem->count > 0) {
            sem->count--;
            mutex_unlock(&sem->lock);
            return 0;
        }
        mutex_unlock(&sem->lock);
        /* Yield to other processes */
        scheduler_yield();
    }
}

/* Try wait (non-blocking) */
int posix_sem_trywait(int sem_id) {
    if (sem_id < 0 || sem_id >= SEM_MAX) return -EINVAL;
    if (!sem_table[sem_id].in_use) return -EINVAL;
    
    struct posix_sem *sem = &sem_table[sem_id];
    mutex_lock(&sem->lock);
    if (sem->count > 0) {
        sem->count--;
        mutex_unlock(&sem->lock);
        return 0;
    }
    mutex_unlock(&sem->lock);
    return -EAGAIN;
}

/* Post (increment) semaphore */
int posix_sem_post(int sem_id) {
    if (sem_id < 0 || sem_id >= SEM_MAX) return -EINVAL;
    if (!sem_table[sem_id].in_use) return -EINVAL;
    
    struct posix_sem *sem = &sem_table[sem_id];
    mutex_lock(&sem->lock);
    sem->count++;
    mutex_unlock(&sem->lock);
    return 0;
}

/* Get semaphore value */
int posix_sem_getvalue(int sem_id, int *sval) {
    if (sem_id < 0 || sem_id >= SEM_MAX) return -EINVAL;
    if (!sem_table[sem_id].in_use) return -EINVAL;
    if (!sval) return -EINVAL;
    
    struct posix_sem *sem = &sem_table[sem_id];
    mutex_lock(&sem->lock);
    *sval = sem->count;
    mutex_unlock(&sem->lock);
    return 0;
}
