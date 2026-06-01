#define KERNEL_INTERNAL
#include "file_lock.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "process.h"
#include "mutex.h"

/* Advisory file locking table */
#define FILE_LOCK_MAX 64

struct file_lock_entry {
    char path[128];
    struct file_lock flk;
    int in_use;
};

static struct file_lock_entry lock_table[FILE_LOCK_MAX];
static mutex_t lock_mutex;
static int lock_initialized = 0;

void file_lock_init(void) {
    if (lock_initialized) return;
    memset(lock_table, 0, sizeof(lock_table));
    mutex_init(&lock_mutex);
    lock_initialized = 1;
    kprintf("[OK] file_lock initialized\n");
}

static int lock_conflicts(struct file_lock *a, struct file_lock *b) {
    if (a->l_type == F_UNLCK || b->l_type == F_UNLCK) return 0;
    /* Check if ranges overlap */
    int64_t a_start = a->l_start;
    int64_t a_end = (a->l_len == 0) ? INT64_MAX : a->l_start + a->l_len;
    int64_t b_start = b->l_start;
    int64_t b_end = (b->l_len == 0) ? INT64_MAX : b->l_start + b->l_len;

    if (a_end <= b_start || b_end <= a_start) return 0; /* no overlap */

    /* Write lock conflicts with anything; read lock only conflicts with write */
    if (a->l_type == F_WRLCK || b->l_type == F_WRLCK) return 1;
    return 0;
}

int file_lock_set(const char *path, struct file_lock *flk, int wait) {
    if (!path || !flk) return -EINVAL;
    (void)wait; /* non-blocking for now */

    mutex_lock(&lock_mutex);

    /* Find or create entry for this path */
    struct file_lock_entry *entry = NULL;
    for (int i = 0; i < FILE_LOCK_MAX; i++) {
        if (lock_table[i].in_use && strcmp(lock_table[i].path, path) == 0) {
            entry = &lock_table[i];
            break;
        }
    }

    if (flk->l_type == F_UNLCK) {
        /* Unlock: remove the entry */
        if (entry) {
            memset(entry, 0, sizeof(struct file_lock_entry));
        }
        mutex_unlock(&lock_mutex);
        return 0;
    }

    /* If no entry exists for this path, create one */
    if (!entry) {
        for (int i = 0; i < FILE_LOCK_MAX; i++) {
            if (!lock_table[i].in_use) {
                entry = &lock_table[i];
                strncpy(entry->path, path, sizeof(entry->path) - 1);
                entry->path[sizeof(entry->path) - 1] = '\0';
                entry->in_use = 1;
                break;
            }
        }
        if (!entry) {
            mutex_unlock(&lock_mutex);
            return -ENOLCK;
        }
        memcpy(&entry->flk, flk, sizeof(struct file_lock));
        entry->flk.l_pid = process_get_current() ? (int32_t)process_get_current()->pid : 0;
        mutex_unlock(&lock_mutex);
        return 0;
    }

    /* Check for conflicts with existing lock */
    if (lock_conflicts(&entry->flk, flk)) {
        struct process *cur = process_get_current();
        if (cur && (uint32_t)entry->flk.l_pid == cur->pid) {
            /* Same process can upgrade/downgrade its own lock */
            memcpy(&entry->flk, flk, sizeof(struct file_lock));
            entry->flk.l_pid = (int32_t)cur->pid;
            mutex_unlock(&lock_mutex);
            return 0;
        }
        mutex_unlock(&lock_mutex);
        return -EAGAIN;
    }

    memcpy(&entry->flk, flk, sizeof(struct file_lock));
    struct process *cur = process_get_current();
    entry->flk.l_pid = cur ? (int32_t)cur->pid : 0;
    mutex_unlock(&lock_mutex);
    return 0;
}

int file_lock_get(const char *path, struct file_lock *flk) {
    if (!path || !flk) return -EINVAL;
    mutex_lock(&lock_mutex);

    for (int i = 0; i < FILE_LOCK_MAX; i++) {
        if (lock_table[i].in_use && strcmp(lock_table[i].path, path) == 0) {
            memcpy(flk, &lock_table[i].flk, sizeof(struct file_lock));
            mutex_unlock(&lock_mutex);
            return 0;
        }
    }

    mutex_unlock(&lock_mutex);
    return -ENOENT;
}

int file_lock_unlock(const char *path, struct file_lock *flk) {
    flk->l_type = F_UNLCK;
    return file_lock_set(path, flk, 0);
}

int file_lock_check(const char *path, struct file_lock *flk) {
    return file_lock_get(path, flk);
}

/* VFS-level wrappers */
int vfs_setlk(const char *path, struct file_lock *flk, int wait) {
    return file_lock_set(path, flk, wait);
}

int vfs_getlk(const char *path, struct file_lock *flk) {
    return file_lock_get(path, flk);
}
