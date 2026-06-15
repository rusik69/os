#ifndef USER_NAMESPACE_H
#define USER_NAMESPACE_H

#include "types.h"
#include "process.h"

/*
 * ── User Namespace (Item 114) ──────────────────────────────────────
 *
 * A user namespace isolates UID and GID numbering.  A process inside
 * a user namespace sees a private set of UIDs/GIDs (including the
 * ability to have UID 0 inside the namespace), which are mapped to
 * a range of UIDs/GIDs in the parent namespace (or the initial user
 * namespace).
 *
 * Enhanced with:
 *   - Full uid_map/gid_map support (single-entry, double-entry)
 *   - chown inside namespace to mapped UIDs
 *   - mknod (only for mapped devices)
 *   - mount inside user namespace
 *   - Capability checks against owning namespace
 *   - Setgroups control based on gid_map write privilege
 *   - /proc/<pid>/uid_map, /proc/<pid>/gid_map
 */

#define USERNS_MAX_NS      64   /* maximum number of user namespaces */
#define USERNS_MAX_MAPS     8   /* max UID/GID map entries per namespace */

/* ── UID/GID map entry ────────────────────────────────────────────
 *
 * Maps a contiguous range of `count` IDs starting at `first_inside`
 * (inside the namespace) to IDs starting at `first_outside`
 * (in the parent namespace).
 */
struct uid_gid_map_entry {
    uint32_t first_inside;     /* first UID/GID inside this namespace */
    uint32_t first_outside;    /* first UID/GID in the parent namespace */
    uint32_t count;            /* number of IDs in the mapping */
};

/* ── User namespace descriptor ──────────────────────────────────── */
struct user_namespace {
    int      id;               /* namespace ID (0 = root/initial) */
    int      in_use;           /* 1 = allocated */

    /* Parent namespace (NULL for the initial user namespace) */
    struct user_namespace *parent;

    /* Level of nesting (0 = initial, max nesting ~32) */
    int      level;

    /* UID mapping table (inside → outside) */
    int                         uid_map_count;
    struct uid_gid_map_entry    uid_map[USERNS_MAX_MAPS];

    /* GID mapping table (inside → outside) */
    int                         gid_map_count;
    struct uid_gid_map_entry    gid_map[USERNS_MAX_MAPS];

    /* The owner UID in the parent namespace — the UID of the process
     * that created this namespace.  This UID effectively owns all
     * mapped UIDs inside the namespace for resource-accounting purposes. */
    uint32_t owner_uid;         /* owner UID in parent namespace */
    uint32_t owner_gid;         /* owner GID in parent namespace */

    /* Number of processes currently in this namespace */
    int      process_count;

    /* Setgroups control: 0 = allow setgroups, 1 = deny setgroups.
     * Set to 1 when a non-single-entry GID mapping is written. */
    int      setgroups_denied;
};

/* ── Root (initial) user namespace ──────────────────────────────── */
extern struct user_namespace init_user_ns;

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the root user namespace (called once at boot) */
void user_ns_init(void);

/* Create a new user namespace.
 * The caller's current UID/GID is mapped to UID 0 / GID 0 inside.
 * Returns NULL on failure (table full or nesting limit reached). */
struct user_namespace *user_ns_create(struct user_namespace *parent,
                                      uint32_t caller_uid,
                                      uint32_t caller_gid);

/* Destroy a user namespace (must be empty of processes). */
void user_ns_destroy(struct user_namespace *ns);

/* ── UID/GID translation ──────────────────────────────────────────
 *
 * Translate a UID from inside this namespace to the parent namespace.
 * Returns the translated UID, or (uint32_t)-1 if no mapping exists.
 */
uint32_t user_ns_translate_uid(const struct user_namespace *ns,
                               uint32_t uid_inside);

/* Translate a GID from inside this namespace to the parent namespace. */
uint32_t user_ns_translate_gid(const struct user_namespace *ns,
                               uint32_t gid_inside);

/* Translate a UID from the parent namespace to inside this namespace.
 * Returns the translated UID or (uint32_t)-1 if no mapping exists. */
uint32_t user_ns_reverse_uid(const struct user_namespace *ns,
                             uint32_t uid_outside);

/* Translate a GID from parent to inside. */
uint32_t user_ns_reverse_gid(const struct user_namespace *ns,
                             uint32_t gid_outside);

/* ── Map manipulation ─────────────────────────────────────────────
 *
 * Add a UID mapping to the namespace.  The calling process must have
 * CAP_SETUID in the parent namespace.  Maps `count` UIDs starting at
 * `first_inside` (inside the ns) to `first_outside` (in the parent ns).
 *
 * Returns 0 on success, -1 on error (table full, overlap, invalid).
 */
int user_ns_add_uid_map(struct user_namespace *ns,
                        uint32_t first_inside,
                        uint32_t first_outside,
                        uint32_t count);

/* Add a GID mapping.  Caller must have CAP_SETGID in parent ns. */
int user_ns_add_gid_map(struct user_namespace *ns,
                        uint32_t first_inside,
                        uint32_t first_outside,
                        uint32_t count);

/* ── Enhanced map operations ──────────────────────────────────────
 *
 * Write full uid_map or gid_map from a formatted string
 * (e.g., "0 1000 1\n500 2000 1\n").
 * The string may contain one or two entries (single-entry or
 * double-entry mapping).
 *
 * Returns 0 on success, -1 on error.
 */
int user_ns_write_uid_map(struct user_namespace *ns, const char *data, uint32_t size);
int user_ns_write_gid_map(struct user_namespace *ns, const char *data, uint32_t size);

/* Read uid_map/gid_map as formatted string (for /proc/PID/uid_map).
 * Returns number of bytes written to buf. */
int user_ns_read_uid_map(const struct user_namespace *ns, char *buf, int buf_size);
int user_ns_read_gid_map(const struct user_namespace *ns, char *buf, int buf_size);

/* ── Setgroups control ────────────────────────────────────────────
 *
 * Allow or deny setgroups syscall inside the user namespace.
 * Denying setgroups is required before writing a non-single-entry
 * GID mapping.
 */
int user_ns_setgroups_allowed(const struct user_namespace *ns);
int user_ns_setgroups_deny(struct user_namespace *ns);

/* ── Capability helpers ───────────────────────────────────────────
 *
 * Check whether `cap` is effective in the given user namespace.
 * Returns 1 if the caller's process has the capability in `ns`.
 */
int user_ns_has_cap(const struct process *proc,
                    struct user_namespace *ns, uint32_t cap);

/* Convenience wrapper — check cap in process's current user ns */
static inline int user_ns_current_has_cap(const struct process *proc,
                                          uint32_t cap)
{
    return user_ns_has_cap(proc, proc ? proc->user_ns : NULL, cap);
}

/* ── Privilege checks for namespace-aware operations ──────────────
 *
 * Check if the current process can chown to a given UID/GID inside
 * its user namespace.  The target UID/GID must be mapped in the
 * namespace's uid_map/gid_map.
 */
int user_ns_can_chown(struct user_namespace *ns, uint32_t target_uid, uint32_t target_gid);

/* Check if the current process can create a device node (mknod)
 * with the given major/minor inside its user namespace.
 * Only mapped devices are allowed. */
int user_ns_can_mknod(struct user_namespace *ns, uint32_t dev_major, uint32_t dev_minor);

/* Check if the current process can mount filesystems inside its
 * user namespace.  Requires CAP_SYS_ADMIN in the owning namespace
 * and certain filesystem types. */
int user_ns_can_mount(struct user_namespace *ns, const char *fstype);

/* ── Filesystem helpers ─────────────────────────────────────────── */
uint32_t user_ns_sb_uid(const struct user_namespace *ns, uint32_t uid);
uint32_t user_ns_sb_gid(const struct user_namespace *ns, uint32_t gid);

/* Get the root (initial) user namespace */
static inline struct user_namespace *user_ns_root(void)
{
    return &init_user_ns;
}

/* Check if a user namespace is the root (initial) namespace */
static inline int user_ns_is_root(const struct user_namespace *ns)
{
    return (ns == &init_user_ns || (ns && ns->level == 0));
}

#endif /* USER_NAMESPACE_H */
