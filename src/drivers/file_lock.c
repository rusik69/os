#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "list.h"
#include "spinlock.h"
#include "file_lock.h"
static spinlock_t flock_lock;
static struct list_head flock_list;
void file_lock_init(void) {
    spinlock_init(&flock_lock);
    list_init(&flock_list);
}
int file_lock_set(const char *path, int type) {
    if (!path) return -1;
    kprintf("[flock] lock %s type=%d\n", path, type);
    return 0;
}
int file_lock_unlock(const char *path) {
    if (!path) return -1;
    kprintf("[flock] unlock %s\n", path);
    return 0;
}
