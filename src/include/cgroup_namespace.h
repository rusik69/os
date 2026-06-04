#ifndef CGROUP_NAMESPACE_H
#define CGROUP_NAMESPACE_H

#include "types.h"

/*
 * cgroup_namespace.h — Cgroup namespace (Item 117)
 *
 * Each cgroup namespace has a root path.  Processes inside the namespace
 * see their cgroup path relative to this root.  The default root is "/"
 * (the full system cgroup hierarchy).  When a new cgroup namespace is
 * created via clone(CLONE_NEWCGROUP) or unshare(CLONE_NEWCGROUP), the
 * current process's cgroup path at the time of creation becomes the
 * namespace root.
 *
 * Multiple processes can share a cgroup namespace after setns(); the
 * namespace is reference-counted and freed when the last user exits.
 */

#define CGROUP_NS_ROOT_LEN 128  /* max length of a cgroup namespace root path */

struct cgroup_namespace {
    int   in_use;                     /* slot active flag */
    int   refcount;                   /* number of processes using this ns */
    char  root_path[CGROUP_NS_ROOT_LEN];  /* virtualized cgroup root */
};

/* Maximum number of cgroup namespaces */
#define CGROUP_NS_MAX 64

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Initialize the cgroup namespace subsystem (called once at boot) */
void cgroup_ns_init(void);

/* Create a new cgroup namespace rooted at the given path.
 * Returns a pointer to the namespace, or NULL on failure. */
struct cgroup_namespace *cgroup_ns_create(const char *current_cgroup_path);

/* Get a reference (increment refcount).  Returns the same pointer. */
struct cgroup_namespace *cgroup_ns_get(struct cgroup_namespace *ns);

/* Release a reference.  If refcount drops to zero, the namespace is freed. */
void cgroup_ns_put(struct cgroup_namespace *ns);

/* Produce the namespace-virtualized cgroup path.
 * Writes the path relative to the namespace root into 'out' (up to 'max' bytes).
 * If ns is NULL (no cgroup namespace), writes the full path. */
void cgroup_ns_get_path(const struct cgroup_namespace *ns,
                        const char *full_path,
                        char *out, int max);

/* Compute a unique inode number for a cgroup namespace.
 * Used by /proc/PID/ns/cgroup to distinguish namespaces. */
uint64_t cgroup_ns_inode(const struct cgroup_namespace *ns);

#endif /* CGROUP_NAMESPACE_H */
