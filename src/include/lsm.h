#ifndef LSM_H
#define LSM_H

/*
 * lsm.h — Linux Security Module hook registration framework.
 *
 * A small, generic hook registry backed by a static_key per hook so that
 * hooks with no registered handler are a single cheap branch (the jump
 * table entry is patched by the jump-label subsystem once enabled).  Each
 * hook slot holds an ordered list of handlers; they are evaluated in
 * registration order and the first non-zero return short-circuits the
 * rest (sequential evaluation, first-fail semantics used by LSM stacking).
 *
 * The framework only dispatches — it does not decide policy.  Enforcement
 * for each hook type lives in the security modules (landlock, smack, yama,
 * ...) which register their callbacks here.
 */

#include "jump_label.h"
#include "types.h"

/* Hook identifiers.  Keep in sync with lsm_hook_names[] in lsm.c. */
enum lsm_hook_id {
    LSM_HOOK_FILE_PERMISSION = 0, /* int (*)(const char *path, int mask) */
    LSM_HOOK_INODE_PERMISSION,    /* int (*)(const char *path, int mask) */
    LSM_HOOK_TASK_ALLOC,          /* int (*)(uint32_t pid) */
    LSM_HOOK_TASK_FREE,           /* void (*)(uint32_t pid) */
    LSM_HOOK_SOCKET_CREATE,       /* int (*)(int domain, int type, int protocol) */
    LSM_HOOK_SOCKET_CONNECT,      /* int (*)(int domain, int type, int protocol) */
    LSM_HOOK_MOUNT,               /* int (*)(const char *src, const char *dest) */
    LSM_HOOK_UMOUNT,              /* int (*)(const char *dest) */
    LSM_HOOK_BPRM_CHECK_SECURITY, /* int (*)(const char *filename) */
    LSM_HOOK_COUNT
};

/* Maximum number of stacked handlers per hook slot. */
#define LSM_MAX_HANDLERS 8

/* A hook slot: ordered handler list + a static key guarding fast path. */
struct lsm_hook_slot {
    struct static_key key;            /* enabled when count > 0 */
    const char *name;                 /* human-readable label for debug */
    int count;                        /* number of registered handlers */
    void *handlers[LSM_MAX_HANDLERS]; /* raw fn pointers (cast per hook) */
};

/* ── Framework API ─────────────────────────────────────────────── */

/* Initialize the LSM framework (called once during kernel boot). */
void lsm_init(void);

/* Register a handler for a hook.  'fn' must cast cleanly to the hook's
 * declared signature.  Returns 0 on success, -EINVAL on bad arg, -ENOSPC
 * if the slot is full.  Once the framework is disabled (see lsm_disable),
 * further registrations are rejected. */
int lsm_register_hook(enum lsm_hook_id id, void *fn);

/* Remove a handler (by pointer identity) from a hook.  Returns 0 if found
 * and removed, -ENOENT otherwise. */
int lsm_unregister_hook(enum lsm_hook_id id, void *fn);

/* True if any handler is registered for a hook (fast check). */
int lsm_hook_active(enum lsm_hook_id id);

/* Disable the whole framework (drop all registrations and keys).  Used by
 * callers that want to permanently turn off LSM enforcement. */
void lsm_disable(void);

/* Return the hook name or "<unknown>". */
const char *lsm_hook_name(enum lsm_hook_id id);

/* ── Typed dispatchers ────────────────────────────────────────── */
/* Each walks the slot in registration order, calling every handler and
 * returning the first non-zero result (0 if all pass).  The static_key
 * gives a fast no-op path when the hook has no handlers. */

int lsm_file_permission(const char *path, int mask);
int lsm_inode_permission(const char *path, int mask);
int lsm_task_alloc(uint32_t pid);
void lsm_task_free(uint32_t pid);
int lsm_socket_create(int domain, int type, int protocol);
int lsm_socket_connect(int domain, int type, int protocol);
int lsm_mount(const char *src, const char *dest);
int lsm_umount(const char *dest);
int lsm_bprm_check_security(const char *filename);

#endif /* LSM_H */