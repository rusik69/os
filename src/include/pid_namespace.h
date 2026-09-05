#ifndef PID_NAMESPACE_H
#define PID_NAMESPACE_H

#include "types.h"
#include "process.h"

/*
 * ── PID Namespace ────────────────────────────────────────────────
 *
 * Each PID namespace has its own PID numbering space.  A process
 * inside a namespace sees its own PIDs starting from 1 (init).
 * Outside the namespace, processes are identified by their global
 * (kernel-wide) PIDs.
 *
 * Namespaces form a hierarchy: each ns (except the root) has a
 * parent.  A process in a parent namespace can see processes in
 * child namespaces, but a process in a child namespace cannot see
 * processes outside its own namespace.
 *
 * Item 111 — PID namespace isolation.
 */

#define PIDNS_MAX_NS  64   /* maximum number of PID namespaces */

/* Forward declaration */
struct process;

/* ── PID namespace descriptor ──────────────────────────────────── */
struct pid_namespace {
    int      id;             /* namespace ID (0 = root/global) */
    int      in_use;         /* 1 = allocated */
    int      level;          /* nesting depth (0 = root) */
    uint32_t parent_id;      /* parent namespace ID (-1 for root) */

    /* Private PID allocator (bitmap) */
#define PIDNS_PID_BITMAP_WORDS  4  /* up to 256 PIDs per namespace */
    uint64_t pid_bitmap[PIDNS_PID_BITMAP_WORDS];

    uint32_t last_allocated; /* last allocated PID (hint for fast path) */

    /* Number of processes currently in this namespace */
    int      process_count;
};

/* ── Root (initial) PID namespace ──────────────────────────────── */
extern struct pid_namespace init_pid_ns;

/* ── API ───────────────────────────────────────────────────────── */

/* Initialize the root PID namespace */
void pid_ns_init(void);

/* Allocate a new PID namespace.  Returns NULL on failure. */
struct pid_namespace *pid_ns_create(struct pid_namespace *parent);

/* Destroy a PID namespace (must be empty of processes). */
void pid_ns_destroy(struct pid_namespace *ns);

/* Allocate a PID within a namespace.  Returns 0 on failure (no free PIDs). */
uint32_t pid_ns_alloc_pid(struct pid_namespace *ns);

/* Free a PID within a namespace. */
void pid_ns_free_pid(struct pid_namespace *ns, uint32_t pid);

/* Get the root (global) PID namespace */
static inline struct pid_namespace *pid_ns_root(void)
{
    return &init_pid_ns;
}

/* Check whether the target process is visible from the caller's PID namespace.
 * Returns 1 if visible, 0 if hidden. */
int pid_ns_visible(const struct process *caller, const struct process *target);

/* Return the namespace-local PID for a process (as seen from its own ns).
 * For the root namespace, this is the same as the global PID. */
uint32_t pid_ns_get_ns_pid(const struct process *proc);

/* ── Cross-namespace PID translation ─────────────────────────────
 *
 * Processes in different PID namespaces address each other with
 * different numbers.  `pid_ns_translate_pid()` converts a target
 * process into the PID value that a process inside `viewer_ns` uses to
 * refer to it; `pid_ns_lookup_pid()` does the reverse — given a PID
 * number as seen inside `caller_ns`, find the underlying process.
 *
 * Translation model (simplified, matches the existing allocator):
 *   - A process is visible from `viewer_ns` only when its own namespace
 *     is `viewer_ns` itself or a descendant of it (a child ns cannot
 *     see ancestors).
 *   - When `viewer_ns` equals the process's own namespace, the viewer
 *     uses the namespace-local PID (`ns_pid`).
 *   - When `viewer_ns` is an ancestor namespace (including the root),
 *     the process is addressed by its global kernel-wide PID, which is
 *     its stable identity at every level above its own namespace.
 *   - Returns 0 when the target is not visible from `viewer_ns`.
 */

/* Is `viewer` the same namespace as, or an ancestor of, `target`? */
int pid_ns_is_ancestor_or_equal(const struct pid_namespace *viewer,
                                const struct pid_namespace *target);

/* Translate `target`'s PID into the number `viewer_ns` uses for it.
 * Returns 0 if `target` is not visible from `viewer_ns`. */
uint32_t pid_ns_translate_pid(const struct process *target, const struct pid_namespace *viewer_ns);

/* Reverse translation: find the process a caller in `caller_ns`
 * addresses with the namespace-local value `pid`.  Returns NULL if no
 * visible process matches. */
struct process *pid_ns_lookup_pid(const struct pid_namespace *caller_ns, uint32_t pid);

#endif /* PID_NAMESPACE_H */
