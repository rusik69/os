#define KERNEL_INTERNAL
#include "types.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "vfs.h"
#include "timer.h"

/* ── Chroot jail restrictions ────────────────────────────────────────── */

/* The chroot path for the current process (NULL = no chroot).
   Stored in process struct's cwd is used as chroot path when
   chroot_enabled is set. We add a per-process chroot_root field
   by extending the concept. */

/* Per-process chroot path (stored separately from cwd) */
#define CHROOT_PATH_MAX 64

static char process_chroot[PROCESS_MAX][CHROOT_PATH_MAX];
static int chroot_initialized = 0;

void chroot_init(void) {
    if (chroot_initialized) return;
    memset(process_chroot, 0, sizeof(process_chroot));
    chroot_initialized = 1;
    kprintf("[OK] chroot initialized\n");
}

/* Set chroot for current process */
int chroot_set(const char *path) {
    if (!path) return -EINVAL;

    struct process *cur = process_get_current();
    if (!cur) return -ESRCH;

    uint32_t pid = cur->pid;
    if (pid >= PROCESS_MAX) return -EINVAL;

    strncpy(process_chroot[pid], path, CHROOT_PATH_MAX - 1);
    process_chroot[pid][CHROOT_PATH_MAX - 1] = '\0';

    kprintf("[chroot] pid=%u chroot to '%s'\n", pid, path);
    return 0;
}

/* Get chroot for a PID (returns NULL if none) */
const char *chroot_get(uint32_t pid) {
    if (pid >= PROCESS_MAX) return NULL;
    if (process_chroot[pid][0] == '\0') return NULL;
    return process_chroot[pid];
}

/* Check if a path access is allowed under chroot.
 * If process is chrooted, path must start with the chroot path.
 * Returns 0 if allowed, -EPERM if denied.
 * For absolute paths, this prepends the chroot.
 */
int chroot_check_path(const char *path, char *resolved, int resolved_max) {
    if (!path || !resolved) return -EINVAL;

    struct process *cur = process_get_current();
    if (!cur) { 
        strncpy(resolved, path, resolved_max);
        return 0;
    }

    const char *root = chroot_get(cur->pid);
    if (!root) {
        /* No chroot — pass through */
        strncpy(resolved, path, resolved_max);
        return 0;
    }

    /* Path is inside chroot if it starts with root,
     * or if it's absolute it should be relative to root */
    if (path[0] == '/') {
        /* Absolute path: prepend chroot root */
        int root_len = (int)strlen(root);
        if (root_len + (int)strlen(path) + 1 > resolved_max)
            return -ENAMETOOLONG;

        memcpy(resolved, root, root_len);
        memcpy(resolved + root_len, path, strlen(path) + 1);
    } else {
        /* Relative path: use as-is (already relative to chroot) */
        strncpy(resolved, path, resolved_max);
    }

    /* Check that resolved path starts with chroot root */
    if (strncmp(resolved, root, strlen(root)) != 0) {
        return -EPERM;
    }

    return 0;
}

/* VFS wrapper for chroot-aware path resolution */
int vfs_chroot_resolve(const char *path, char *out, int out_max) {
    return chroot_check_path(path, out, out_max);
}

/* ── Stub: chroot_get ─────────────────────────────── */
int chroot_get(char *buf, size_t size)
{
    (void)buf;
    (void)size;
    kprintf("[chroot] chroot_get: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: chroot_escape ─────────────────────────────── */
int chroot_escape(void *task)
{
    (void)task;
    kprintf("[chroot] chroot_escape: not yet implemented\n");
    return -ENOSYS;
}
