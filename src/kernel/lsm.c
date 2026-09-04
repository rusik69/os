/*
 * lsm.c — LSM hook registration framework.
 *
 * Implements a generic hook registry used by security modules.  Each hook
 * slot keeps an ordered list of handlers and a static_key; the static key
 * is enabled on the first registration so jump-label patching turns the
 * (common) no-handler case into a single fall-through branch.  Dispatch
 * evaluates handlers sequentially and stops at the first error — the
 * standard LSM "stacking, first-fail" ordering established here and later
 * extended by the dedicated stacking layer.
 */

#define KERNEL_INTERNAL
#include "lsm.h"

#include "atomic.h"
#include "errno.h"
#include "printf.h"

/* One global slot per hook. */
static struct lsm_hook_slot lsm_hooks[LSM_HOOK_COUNT];

static const char *lsm_hook_names[LSM_HOOK_COUNT] = {
    "file_permission", "inode_permission", "task_alloc",
    "task_free",       "socket_create",    "socket_connect",
    "mount",           "umount",           "bprm_check_security",
};

static int lsm_initialized;
static int lsm_enabled = 1;

void lsm_init(void) {
    int i;

    if (lsm_initialized)
        return;

    for (i = 0; i < LSM_HOOK_COUNT; i++) {
        memset(&lsm_hooks[i], 0, sizeof(lsm_hooks[i]));
        static_key_init(&lsm_hooks[i].key);
        lsm_hooks[i].name = lsm_hook_names[i];
    }

    lsm_initialized = 1;
    lsm_enabled = 1;
    kprintf("[OK] lsm: hook framework ready (%d hook types)\n", LSM_HOOK_COUNT);
}

const char *lsm_hook_name(enum lsm_hook_id id) {
    if (id < 0 || id >= LSM_HOOK_COUNT)
        return "<unknown>";
    return lsm_hook_names[id];
}

int lsm_register_hook(enum lsm_hook_id id, void *fn) {
    struct lsm_hook_slot *slot;

    if (!lsm_initialized)
        return -EINVAL;
    if (id < 0 || id >= LSM_HOOK_COUNT || !fn)
        return -EINVAL;
    if (!lsm_enabled)
        return -EINVAL;

    slot = &lsm_hooks[id];

    /* Reject duplicate registrations of the same handler. */
    for (int i = 0; i < slot->count; i++)
        if (slot->handlers[i] == fn)
            return 0;

    if (slot->count >= LSM_MAX_HANDLERS)
        return -ENOSPC;

    slot->handlers[slot->count++] = fn;
    if (slot->count == 1)
        static_key_enable(&slot->key);

    return 0;
}

int lsm_unregister_hook(enum lsm_hook_id id, void *fn) {
    struct lsm_hook_slot *slot;
    int i;

    if (id < 0 || id >= LSM_HOOK_COUNT || !fn)
        return -EINVAL;

    slot = &lsm_hooks[id];
    for (i = 0; i < slot->count; i++) {
        if (slot->handlers[i] == fn) {
            /* Shift remaining handlers down. */
            for (int j = i; j < slot->count - 1; j++)
                slot->handlers[j] = slot->handlers[j + 1];
            slot->count--;
            if (slot->count == 0)
                static_key_disable(&slot->key);
            return 0;
        }
    }
    return -ENOENT;
}

int lsm_hook_active(enum lsm_hook_id id) {
    if (id < 0 || id >= LSM_HOOK_COUNT)
        return 0;
    return lsm_hooks[id].count > 0;
}

void lsm_disable(void) {
    int i;

    if (!lsm_initialized)
        return;

    lsm_enabled = 0;
    for (i = 0; i < LSM_HOOK_COUNT; i++) {
        memset(lsm_hooks[i].handlers, 0, sizeof(lsm_hooks[i].handlers));
        lsm_hooks[i].count = 0;
        static_key_disable(&lsm_hooks[i].key);
    }
    kprintf("[OK] lsm: framework disabled\n");
}

/* ── Typed dispatchers ────────────────────────────────────────── */

int lsm_file_permission(const char *path, int mask) {
    struct lsm_hook_slot *slot = &lsm_hooks[LSM_HOOK_FILE_PERMISSION];

    if (!lsm_enabled || !atomic_read(&slot->key.enabled) || slot->count == 0)
        return 0;

    for (int i = 0; i < slot->count; i++) {
        typedef int (*fn_t)(const char *, int);
        fn_t fn = (fn_t)slot->handlers[i];
        int ret = fn(path, mask);
        if (ret != 0)
            return ret;
    }
    return 0;
}

int lsm_inode_permission(const char *path, int mask) {
    struct lsm_hook_slot *slot = &lsm_hooks[LSM_HOOK_INODE_PERMISSION];

    if (!lsm_enabled || !atomic_read(&slot->key.enabled) || slot->count == 0)
        return 0;

    for (int i = 0; i < slot->count; i++) {
        typedef int (*fn_t)(const char *, int);
        fn_t fn = (fn_t)slot->handlers[i];
        int ret = fn(path, mask);
        if (ret != 0)
            return ret;
    }
    return 0;
}

int lsm_task_alloc(uint32_t pid) {
    struct lsm_hook_slot *slot = &lsm_hooks[LSM_HOOK_TASK_ALLOC];

    if (!lsm_enabled || !atomic_read(&slot->key.enabled) || slot->count == 0)
        return 0;

    for (int i = 0; i < slot->count; i++) {
        typedef int (*fn_t)(uint32_t);
        fn_t fn = (fn_t)slot->handlers[i];
        int ret = fn(pid);
        if (ret != 0)
            return ret;
    }
    return 0;
}

void lsm_task_free(uint32_t pid) {
    struct lsm_hook_slot *slot = &lsm_hooks[LSM_HOOK_TASK_FREE];

    if (!lsm_enabled || !atomic_read(&slot->key.enabled) || slot->count == 0)
        return;

    for (int i = 0; i < slot->count; i++) {
        typedef void (*fn_t)(uint32_t);
        fn_t fn = (fn_t)slot->handlers[i];
        fn(pid);
    }
}

int lsm_socket_create(int domain, int type, int protocol) {
    struct lsm_hook_slot *slot = &lsm_hooks[LSM_HOOK_SOCKET_CREATE];

    if (!lsm_enabled || !atomic_read(&slot->key.enabled) || slot->count == 0)
        return 0;

    for (int i = 0; i < slot->count; i++) {
        typedef int (*fn_t)(int, int, int);
        fn_t fn = (fn_t)slot->handlers[i];
        int ret = fn(domain, type, protocol);
        if (ret != 0)
            return ret;
    }
    return 0;
}

int lsm_socket_connect(int domain, int type, int protocol) {
    struct lsm_hook_slot *slot = &lsm_hooks[LSM_HOOK_SOCKET_CONNECT];

    if (!lsm_enabled || !atomic_read(&slot->key.enabled) || slot->count == 0)
        return 0;

    for (int i = 0; i < slot->count; i++) {
        typedef int (*fn_t)(int, int, int);
        fn_t fn = (fn_t)slot->handlers[i];
        int ret = fn(domain, type, protocol);
        if (ret != 0)
            return ret;
    }
    return 0;
}

int lsm_mount(const char *src, const char *dest) {
    struct lsm_hook_slot *slot = &lsm_hooks[LSM_HOOK_MOUNT];

    if (!lsm_enabled || !atomic_read(&slot->key.enabled) || slot->count == 0)
        return 0;

    for (int i = 0; i < slot->count; i++) {
        typedef int (*fn_t)(const char *, const char *);
        fn_t fn = (fn_t)slot->handlers[i];
        int ret = fn(src, dest);
        if (ret != 0)
            return ret;
    }
    return 0;
}

int lsm_umount(const char *dest) {
    struct lsm_hook_slot *slot = &lsm_hooks[LSM_HOOK_UMOUNT];

    if (!lsm_enabled || !atomic_read(&slot->key.enabled) || slot->count == 0)
        return 0;

    for (int i = 0; i < slot->count; i++) {
        typedef int (*fn_t)(const char *);
        fn_t fn = (fn_t)slot->handlers[i];
        int ret = fn(dest);
        if (ret != 0)
            return ret;
    }
    return 0;
}

int lsm_bprm_check_security(const char *filename) {
    struct lsm_hook_slot *slot = &lsm_hooks[LSM_HOOK_BPRM_CHECK_SECURITY];

    if (!lsm_enabled || !atomic_read(&slot->key.enabled) || slot->count == 0)
        return 0;

    for (int i = 0; i < slot->count; i++) {
        typedef int (*fn_t)(const char *);
        fn_t fn = (fn_t)slot->handlers[i];
        int ret = fn(filename);
        if (ret != 0)
            return ret;
    }
    return 0;
}