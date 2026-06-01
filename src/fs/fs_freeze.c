#define KERNEL_INTERNAL
#include "types.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "process.h"
#include "mutex.h"

/* ── Filesystem freeze — prevent modifications to a filesystem ───────── *
 * Allows freezing a mounted filesystem for snapshot/backup consistency.
 */

#define FS_FREEZE_MAX 8

struct fs_freeze_entry {
    char mountpoint[64];
    int frozen;
    uint32_t freezer_pid;
    int in_use;
};

static struct fs_freeze_entry freeze_table[FS_FREEZE_MAX];
static mutex_t freeze_mutex;
static int fs_freeze_initialized = 0;

void fs_freeze_init(void) {
    if (fs_freeze_initialized) return;
    memset(freeze_table, 0, sizeof(freeze_table));
    mutex_init(&freeze_mutex);
    fs_freeze_initialized = 1;
    kprintf("[OK] fs_freeze initialized\n");
}

/* Freeze a filesystem mounted at the given path */
int fs_freeze(const char *mountpoint) {
    if (!mountpoint) return -EINVAL;
    mutex_lock(&freeze_mutex);
    
    /* Check if already frozen */
    for (int i = 0; i < FS_FREEZE_MAX; i++) {
        if (freeze_table[i].in_use && 
            strcmp(freeze_table[i].mountpoint, mountpoint) == 0) {
            mutex_unlock(&freeze_mutex);
            return -EBUSY;
        }
    }
    
    /* Find free slot */
    for (int i = 0; i < FS_FREEZE_MAX; i++) {
        if (!freeze_table[i].in_use) {
            strncpy(freeze_table[i].mountpoint, mountpoint, sizeof(freeze_table[i].mountpoint) - 1);
            freeze_table[i].mountpoint[sizeof(freeze_table[i].mountpoint) - 1] = '\0';
            freeze_table[i].frozen = 1;
            struct process *cur = process_get_current();
            freeze_table[i].freezer_pid = cur ? cur->pid : 0;
            freeze_table[i].in_use = 1;
            
            kprintf("[fs_freeze] frozen '%s' by pid=%u\n", 
                    mountpoint, freeze_table[i].freezer_pid);
            mutex_unlock(&freeze_mutex);
            return 0;
        }
    }
    
    mutex_unlock(&freeze_mutex);
    return -ENOSPC;
}

/* Thaw (unfreeze) a filesystem */
int fs_thaw(const char *mountpoint) {
    if (!mountpoint) return -EINVAL;
    mutex_lock(&freeze_mutex);
    
    for (int i = 0; i < FS_FREEZE_MAX; i++) {
        if (freeze_table[i].in_use && 
            strcmp(freeze_table[i].mountpoint, mountpoint) == 0) {
            freeze_table[i].in_use = 0;
            freeze_table[i].frozen = 0;
            kprintf("[fs_freeze] thawed '%s'\n", mountpoint);
            mutex_unlock(&freeze_mutex);
            return 0;
        }
    }
    
    mutex_unlock(&freeze_mutex);
    return -ENOENT;
}

/* Check if a path is on a frozen filesystem */
int fs_is_frozen(const char *path) {
    if (!path) return 0;
    mutex_lock(&freeze_mutex);
    
    for (int i = 0; i < FS_FREEZE_MAX; i++) {
        if (freeze_table[i].in_use && freeze_table[i].frozen) {
            /* Check if path is under this mountpoint */
            if (strncmp(path, freeze_table[i].mountpoint, 
                        strlen(freeze_table[i].mountpoint)) == 0) {
                mutex_unlock(&freeze_mutex);
                return 1;
            }
        }
    }
    
    mutex_unlock(&freeze_mutex);
    return 0;
}

/* Check write operation against freeze state */
int fs_check_not_frozen(const char *path) {
    if (fs_is_frozen(path)) {
        kprintf("[fs_freeze] write denied to frozen fs (path=%s)\n", path);
        return -EROFS;
    }
    return 0;
}
