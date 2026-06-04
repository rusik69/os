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

#endif /* PID_NAMESPACE_H */
