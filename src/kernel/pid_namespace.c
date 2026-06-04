/*
 * pid_namespace.c — PID namespace isolation (Item 111)
 *
 * Implements per-process PID namespaces.  Each namespace has its own
 * private PID allocator so that processes inside the namespace see
 * small sequential PIDs starting from 1, while processes outside see
 * the global (kernel-wide) PIDs.
 *
 * Namespace hierarchy:
 *   - The root namespace (init_pid_ns) contains all processes.
 *   - Children created via clone(CLONE_NEWPID) start a new ns.
 *   - Within a child namespace, the first process is PID 1 (init).
 *   - A process in a parent namespace can see child-ns processes.
 *   - A process in a child namespace cannot see parent-ns processes.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "pid_namespace.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"

/* ── Storage for PID namespace table ───────────────────────────── */
static struct pid_namespace pid_ns_table[PIDNS_MAX_NS];
static int pid_ns_count = 0;           /* number of allocated namespaces */
static spinlock_t pid_ns_lock = 0;     /* protects the namespace table */

/* ── Root (global) PID namespace ───────────────────────────────── */
struct pid_namespace init_pid_ns = {
    .id       = 0,
    .in_use   = 1,
    .level    = 0,
    .parent_id = (uint32_t)-1,
    .pid_bitmap = {0},
    .last_allocated = 0,
    .process_count = 0,
};

/* ── Initialization ────────────────────────────────────────────── */

void pid_ns_init(void)
{
    /* Root namespace: PID 0 (idle) is always allocated */
    init_pid_ns.pid_bitmap[0] = 1;  /* PID 0 reserved */
    init_pid_ns.last_allocated = 0;

    /* Mark the root slot as used in the table */
    pid_ns_table[0] = init_pid_ns;
    pid_ns_count = 1;

    kprintf("[OK] pid_namespace: root namespace initialized\n");
}

/* ── Allocate a new PID namespace ──────────────────────────────── */

struct pid_namespace *pid_ns_create(struct pid_namespace *parent)
{
    struct pid_namespace *ns = NULL;

    spinlock_acquire(&pid_ns_lock);

    for (int i = 1; i < PIDNS_MAX_NS; i++) {
        if (!pid_ns_table[i].in_use) {
            ns = &pid_ns_table[i];
            break;
        }
    }

    if (!ns) {
        spinlock_release(&pid_ns_lock);
        kprintf("[PIDNS] Failed to allocate new PID namespace (table full)\n");
        return NULL;
    }

    /* Initialize the namespace */
    memset(ns, 0, sizeof(*ns));
    ns->id = (int)(ns - pid_ns_table);
    ns->in_use = 1;
    ns->level = parent ? (parent->level + 1) : 1;
    ns->parent_id = parent ? (uint32_t)parent->id : (uint32_t)-1;
    ns->pid_bitmap[0] = 1;  /* PID 0 reserved */
    ns->last_allocated = 0;
    ns->process_count = 0;

    pid_ns_count++;

    spinlock_release(&pid_ns_lock);

    kprintf("[PIDNS] New namespace created (id=%d, level=%d, parent=%d)\n",
            ns->id, ns->level, ns->parent_id);

    return ns;
}

/* ── Destroy a PID namespace ───────────────────────────────────── */

void pid_ns_destroy(struct pid_namespace *ns)
{
    if (!ns || ns == &init_pid_ns) return;  /* cannot destroy root */

    spinlock_acquire(&pid_ns_lock);

    if (ns->process_count > 0) {
        kprintf("[PIDNS] WARNING: destroying namespace id=%d with %d processes still present!\n",
                ns->id, ns->process_count);
        /* Force-clear the process count — caller must have migrated them */
    }

    memset(ns, 0, sizeof(*ns));
    pid_ns_count--;

    spinlock_release(&pid_ns_lock);

    kprintf("[PIDNS] Namespace id=%d destroyed\n", ns->id);
}

/* ── Allocate a PID within a namespace ───────────────────────────
 *
 * Returns a PID in the range 1..(PIDNS_PID_BITMAP_WORDS * 64 - 1).
 * PID 0 is reserved and never allocated.
 * Returns 0 on failure (no free PIDs).
 */
uint32_t pid_ns_alloc_pid(struct pid_namespace *ns)
{
    if (!ns) return 0;

    uint32_t pid = 0;
    int start_word = (ns->last_allocated + 1) / 64;

    spinlock_acquire(&pid_ns_lock);

    /* Search from last_allocated+1 to end, then wrap to beginning */
    for (int pass = 0; pass < 2 && pid == 0; pass++) {
        int start = (pass == 0) ? start_word : 0;
        int end   = (pass == 0) ? PIDNS_PID_BITMAP_WORDS : start_word;

        for (int w = start; w < end && pid == 0; w++) {
            if (ns->pid_bitmap[w] == ~0ULL) continue;
            int bit = __builtin_ctzll(~ns->pid_bitmap[w]);
            pid = (uint32_t)(w * 64 + bit);
            if (pid == 0) continue;  /* PID 0 is reserved */
            ns->pid_bitmap[w] |= (1ULL << bit);
            ns->last_allocated = pid;
            ns->process_count++;
        }
    }

    spinlock_release(&pid_ns_lock);

    return pid;
}

/* ── Free a PID within a namespace ─────────────────────────────── */

void pid_ns_free_pid(struct pid_namespace *ns, uint32_t pid)
{
    if (!ns || pid == 0) return;

    int w = pid / 64;
    int bit = pid % 64;

    if (w < 0 || w >= PIDNS_PID_BITMAP_WORDS) return;

    spinlock_acquire(&pid_ns_lock);

    if (ns->pid_bitmap[w] & (1ULL << bit)) {
        ns->pid_bitmap[w] &= ~(1ULL << bit);
        if (ns->process_count > 0)
            ns->process_count--;
    }

    spinlock_release(&pid_ns_lock);
}

/* ── Visibility check ────────────────────────────────────────────
 *
 * A process is visible from the caller's PID namespace if:
 *   1. They are in the same namespace, OR
 *   2. The caller is in a parent namespace of the target, OR
 *   3. The caller is root (uid 0) — sees all
 */
int pid_ns_visible(const struct process *caller, const struct process *target)
{
    if (!caller || !target) return 0;
    if (caller == target) return 1;

    /* Root (euid == 0) sees all processes regardless of namespace */
    if (caller->euid == 0) return 1;

    struct pid_namespace *caller_ns = caller->pid_ns;
    struct pid_namespace *target_ns = target->pid_ns;

    /* If either has no namespace, fall back to global visibility */
    if (!caller_ns || !target_ns) return 1;

    /* Same namespace — visible */
    if (caller_ns == target_ns) return 1;

    /* Caller is in an ancestor namespace — visible */
    struct pid_namespace *ns = target_ns;
    while (ns && ns->parent_id != (uint32_t)-1) {
        if (ns->parent_id == (uint32_t)caller_ns->id)
            return 1;
        /* Walk up: find parent by ID */
        if (ns->id < 0 || ns->id >= PIDNS_MAX_NS) break;
        uint32_t parent_id = ns->parent_id;
        if (parent_id >= PIDNS_MAX_NS) break;
        ns = &pid_ns_table[parent_id];
        if (!ns || !ns->in_use) break;
        if (ns == caller_ns) return 1;  /* reached caller's ns */
    }

    return 0;
}

/* ── Get namespace-local PID ─────────────────────────────────────
 *
 * Inside a PID namespace, the process's visible PID is the one
 * allocated within that namespace.  For the root namespace, this
 * is the same as the global PID.  For child namespaces, the first
 * process gets PID 1 (init).
 *
 * The ns_pid is stored in the process struct and set at creation
 * time.  We look it up here.
 */
uint32_t pid_ns_get_ns_pid(const struct process *proc)
{
    if (!proc) return 0;
    /* The namespace-local PID is stored in the process struct
     * at creation time.  For the root namespace this is the same
     * as proc->pid. */
    if (proc->pid_ns == &init_pid_ns || !proc->pid_ns)
        return proc->pid;  /* global PID = namespace PID for root ns */
    return proc->ns_pid;   /* namespace-local PID */
}
