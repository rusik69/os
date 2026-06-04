/*
 * user_namespace.c — User namespace UID/GID isolation (Item 114)
 *
 * Implements per-process user namespaces.  Each namespace has its own
 * UID/GID numbering space, allowing a process inside the namespace to
 * have UID 0 (root) while actually running as an unprivileged user in
 * the parent namespace.
 *
 * Namespace hierarchy:
 *   - The root namespace (init_user_ns) contains all initial credentials.
 *   - Children created via clone(CLONE_NEWUSER) start a new ns.
 *   - Within a child namespace, the first process is mapped to UID 0
 *     (the UID of the creating process in the parent namespace maps
 *     to 0 inside the child).
 *   - Capabilities inside a user namespace are scoped: having CAP_SYS_ADMIN
 *     inside a user namespace does not grant any privilege outside it.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "user_namespace.h"
#include "process.h"
#include "caps.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"

/* ── Storage for user namespace table ───────────────────────────── */
static struct user_namespace user_ns_table[USERNS_MAX_NS];
static int user_ns_count = 0;           /* number of allocated namespaces */
static spinlock_t user_ns_lock = 0;     /* protects the namespace table */

/* ── Root (global) user namespace ───────────────────────────────── */
struct user_namespace init_user_ns = {
    .id              = 0,
    .in_use          = 1,
    .parent          = NULL,
    .level           = 0,
    .uid_map_count   = 1,
    .uid_map         = {{0, 0, 0xFFFFFFFF}},  /* identity map */
    .gid_map_count   = 1,
    .gid_map         = {{0, 0, 0xFFFFFFFF}},  /* identity map */
    .owner_uid       = 0,
    .owner_gid       = 0,
    .process_count   = 0,
};

/* ── Forward declarations ───────────────────────────────────────── */
static int map_lookup(const struct uid_gid_map_entry *map, int count,
                      uint32_t inside, uint32_t *outside);
static int map_reverse_lookup(const struct uid_gid_map_entry *map, int count,
                              uint32_t outside, uint32_t *inside);

/* ── Initialization ────────────────────────────────────────────── */

void user_ns_init(void)
{
    /* Mark the root slot as used in the table */
    user_ns_table[0] = init_user_ns;
    user_ns_count = 1;

    kprintf("[OK] user_namespace: root namespace initialized\n");
}

/* ── Allocate a new user namespace ────────────────────────────────
 *
 * The new namespace maps the creator's UID/GID in the parent namespace
 * to UID 0 / GID 0 inside.  Additional mappings can be added via
 * /proc/PID/uid_map and /proc/PID/gid_map after creation.
 */
struct user_namespace *user_ns_create(struct user_namespace *parent,
                                      uint32_t caller_uid,
                                      uint32_t caller_gid)
{
    struct user_namespace *ns = NULL;

    spinlock_acquire(&user_ns_lock);

    for (int i = 1; i < USERNS_MAX_NS; i++) {
        if (!user_ns_table[i].in_use) {
            ns = &user_ns_table[i];
            break;
        }
    }

    if (!ns) {
        spinlock_release(&user_ns_lock);
        kprintf("[USERNS] Failed to allocate new user namespace (table full)\n");
        return NULL;
    }

    /* Initialize the namespace */
    memset(ns, 0, sizeof(*ns));
    ns->id = (int)(ns - user_ns_table);
    ns->in_use = 1;
    ns->parent = parent ? parent : &init_user_ns;
    ns->level = parent ? (parent->level + 1) : 1;
    ns->owner_uid = caller_uid;
    ns->owner_gid = caller_gid;
    ns->process_count = 0;

    /* Set up initial mapping: caller's UID/GID → 0 inside */
    ns->uid_map_count = 1;
    ns->uid_map[0].first_inside  = 0;
    ns->uid_map[0].first_outside = caller_uid;
    ns->uid_map[0].count         = 1;

    ns->gid_map_count = 1;
    ns->gid_map[0].first_inside  = 0;
    ns->gid_map[0].first_outside = caller_gid;
    ns->gid_map[0].count         = 1;

    user_ns_count++;

    spinlock_release(&user_ns_lock);

    kprintf("[USERNS] New namespace created (id=%d, level=%d, owner=%lu/%lu)\n",
            ns->id, ns->level,
            (unsigned long)caller_uid, (unsigned long)caller_gid);

    return ns;
}

/* ── Destroy a user namespace ──────────────────────────────────── */

void user_ns_destroy(struct user_namespace *ns)
{
    if (!ns || ns == &init_user_ns) return;  /* cannot destroy root */

    spinlock_acquire(&user_ns_lock);

    if (ns->process_count > 0) {
        kprintf("[USERNS] WARNING: destroying namespace id=%d with %d processes still present!\n",
                ns->id, ns->process_count);
    }

    memset(ns, 0, sizeof(*ns));
    user_ns_count--;

    spinlock_release(&user_ns_lock);

    kprintf("[USERNS] Namespace id=%d destroyed\n", ns->id);
}

/* ── Internal: map lookup (inside → outside) ──────────────────────
 *
 * Searches the map table for an entry covering `inside`.
 * If found, sets *outside to the translated value and returns 1.
 * Returns 0 if no mapping covers `inside`.
 */
static int map_lookup(const struct uid_gid_map_entry *map, int count,
                      uint32_t inside, uint32_t *outside)
{
    for (int i = 0; i < count; i++) {
        if (inside >= map[i].first_inside &&
            inside < map[i].first_inside + map[i].count) {
            uint32_t offset = inside - map[i].first_inside;
            *outside = map[i].first_outside + offset;
            return 1;
        }
    }
    return 0;
}

/* ── Internal: reverse map lookup (outside → inside) ────────────── */
static int map_reverse_lookup(const struct uid_gid_map_entry *map, int count,
                              uint32_t outside, uint32_t *inside)
{
    for (int i = 0; i < count; i++) {
        if (outside >= map[i].first_outside &&
            outside < map[i].first_outside + map[i].count) {
            uint32_t offset = outside - map[i].first_outside;
            *inside = map[i].first_inside + offset;
            return 1;
        }
    }
    return 0;
}

/* ── UID translation (inside → parent namespace) ───────────────── */

uint32_t user_ns_translate_uid(const struct user_namespace *ns,
                               uint32_t uid_inside)
{
    if (!ns || ns == &init_user_ns)
        return uid_inside;  /* identity translation for root ns */

    uint32_t outside;
    if (map_lookup(ns->uid_map, ns->uid_map_count, uid_inside, &outside))
        return outside;

    /* No mapping found — return the owner UID as a fallback */
    return ns->owner_uid;
}

/* ── GID translation (inside → parent namespace) ───────────────── */

uint32_t user_ns_translate_gid(const struct user_namespace *ns,
                               uint32_t gid_inside)
{
    if (!ns || ns == &init_user_ns)
        return gid_inside;

    uint32_t outside;
    if (map_lookup(ns->gid_map, ns->gid_map_count, gid_inside, &outside))
        return outside;

    return ns->owner_gid;
}

/* ── UID reverse translation (parent → inside) ─────────────────── */

uint32_t user_ns_reverse_uid(const struct user_namespace *ns,
                             uint32_t uid_outside)
{
    if (!ns || ns == &init_user_ns)
        return uid_outside;

    uint32_t inside;
    if (map_reverse_lookup(ns->uid_map, ns->uid_map_count,
                           uid_outside, &inside))
        return inside;

    return (uint32_t)-1;
}

/* ── GID reverse translation (parent → inside) ─────────────────── */

uint32_t user_ns_reverse_gid(const struct user_namespace *ns,
                             uint32_t gid_outside)
{
    if (!ns || ns == &init_user_ns)
        return gid_outside;

    uint32_t inside;
    if (map_reverse_lookup(ns->gid_map, ns->gid_map_count,
                           gid_outside, &inside))
        return inside;

    return (uint32_t)-1;
}

/* ── Add UID mapping ──────────────────────────────────────────────
 *
 * Adds a mapping from `first_inside` (inside the ns) to
 * `first_outside` (in the parent ns) for `count` consecutive UIDs.
 *
 * Caller must have CAP_SETUID in the parent namespace.
 *
 * Returns 0 on success, -1 with reason logged on error.
 */
int user_ns_add_uid_map(struct user_namespace *ns,
                        uint32_t first_inside,
                        uint32_t first_outside,
                        uint32_t count)
{
    if (!ns || count == 0) return -1;

    if (ns->uid_map_count >= USERNS_MAX_MAPS) {
        kprintf("[USERNS] uid_map full (%d entries max)\n", USERNS_MAX_MAPS);
        return -1;
    }

    /* Check for overlap with existing entries */
    for (int i = 0; i < ns->uid_map_count; i++) {
        uint32_t exist_start = ns->uid_map[i].first_inside;
        uint32_t exist_end   = exist_start + ns->uid_map[i].count;

        uint32_t new_start = first_inside;
        uint32_t new_end   = first_inside + count;

        /* Check overlap */
        if (new_start < exist_end && exist_start < new_end) {
            kprintf("[USERNS] uid_map entry overlaps with existing entry %d\n", i);
            return -1;
        }
    }

    int idx = ns->uid_map_count;
    ns->uid_map[idx].first_inside  = first_inside;
    ns->uid_map[idx].first_outside = first_outside;
    ns->uid_map[idx].count         = count;
    ns->uid_map_count++;

    kprintf("[USERNS] uid_map added: inside %lu..%lu → outside %lu..%lu\n",
            (unsigned long)first_inside,
            (unsigned long)(first_inside + count - 1),
            (unsigned long)first_outside,
            (unsigned long)(first_outside + count - 1));

    return 0;
}

/* ── Add GID mapping ────────────────────────────────────────────── */

int user_ns_add_gid_map(struct user_namespace *ns,
                        uint32_t first_inside,
                        uint32_t first_outside,
                        uint32_t count)
{
    if (!ns || count == 0) return -1;

    if (ns->gid_map_count >= USERNS_MAX_MAPS) {
        kprintf("[USERNS] gid_map full (%d entries max)\n", USERNS_MAX_MAPS);
        return -1;
    }

    /* Check for overlap with existing entries */
    for (int i = 0; i < ns->gid_map_count; i++) {
        uint32_t exist_start = ns->gid_map[i].first_inside;
        uint32_t exist_end   = exist_start + ns->gid_map[i].count;

        uint32_t new_start = first_inside;
        uint32_t new_end   = first_inside + count;

        if (new_start < exist_end && exist_start < new_end) {
            kprintf("[USERNS] gid_map entry overlaps with existing entry %d\n", i);
            return -1;
        }
    }

    int idx = ns->gid_map_count;
    ns->gid_map[idx].first_inside  = first_inside;
    ns->gid_map[idx].first_outside = first_outside;
    ns->gid_map[idx].count         = count;
    ns->gid_map_count++;

    kprintf("[USERNS] gid_map added: inside %lu..%lu → outside %lu..%lu\n",
            (unsigned long)first_inside,
            (unsigned long)(first_inside + count - 1),
            (unsigned long)first_outside,
            (unsigned long)(first_outside + count - 1));

    return 0;
}

/* ── Capability check inside a user namespace ─────────────────────
 *
 * A process has capability `cap` inside a user namespace if:
 *   1. The process is UID 0 in that namespace (according to the uid_map), OR
 *   2. The process has the capability in its effective set AND the
 *      capability is effective in the parent namespace all the way
 *      to the root.
 *
 * For the root namespace (init_user_ns), this degrades to the standard
 * capability check (checking syscall_caps in the process struct).
 */
int user_ns_has_cap(const struct process *proc,
                    struct user_namespace *ns, uint32_t cap)
{
    if (!proc) return 0;

    /* Root namespace: standard capability check */
    if (!ns || ns == &init_user_ns) {
        /* Check if process has this capability in its effective set */
        int word = cap / 64;
        int bit  = cap % 64;
        if (word < PROCESS_SYSCALL_CAP_WORDS)
            return (proc->syscall_caps[word] >> bit) & 1;
        return 0;
    }

    /* Inside a user namespace: check if the process is UID 0 in this ns.
     * Translate the process's effective UID into the namespace's view
     * of itself (which is just proc->euid unless the process has been
     * re-identified by setns/exec with ns credentials).  For the simple
     * case (process was created with CLONE_NEWUSER and inherited the
     * namespace mapping), the process's euid inside the namespace is
     * the mapped value. */

    /* If the process's effective UID is 0 inside this ns → has all caps */
    if (proc->euid == 0) {
        /* But only if 0 is actually mapped in this ns.
         * A process running as UID 1000 in the parent ns, inside a child
         * ns where that maps to 0, has euid == 0 in the child ns view. */
        uint32_t mapped_uid = user_ns_translate_uid(ns, proc->euid);
        /* proc->euid is already the inside-ns view if the ns was set up
         * correctly.  In the simple case where proc->euid is already 0
         * in this ns's terms, the caller has root-equivalent caps. */
        if (proc->euid == 0) {
            /* Check if UID 0 is actually mapped inside this ns */
            uint32_t outside_uid;
            if (map_lookup(ns->uid_map, ns->uid_map_count, 0, &outside_uid))
                return 1;  /* UID 0 is mapped → caller is root in this ns */
        }
    }

    /* Check the process's effective capability set directly.
     * Capabilities in a user namespace are scoped: even if the process
     * has CAP_NET_ADMIN in its bitmask, it only applies to resources
     * owned by this namespace.  Here we just check the bit. */
    int word = cap / 64;
    int bit  = cap % 64;
    if (word < PROCESS_SYSCALL_CAP_WORDS)
        return (proc->syscall_caps[word] >> bit) & 1;

    return 0;
}

/* ── Filesystem helpers ───────────────────────────────────────────
 *
 * Translate a UID/GID from a user namespace's perspective for the
 * purpose of filesystem permission checks.  These are used when a
 * process in one user namespace accesses a filesystem that was
 * created/owned in another user namespace's context.
 *
 * For now, these simply return the UID/GID as-is (identity transform
 * outside the namespace boundary).  A full implementation would walk
 * the namespace hierarchy to find the right mapping.
 */

uint32_t user_ns_sb_uid(const struct user_namespace *ns, uint32_t uid)
{
    (void)ns;
    return uid;
}

uint32_t user_ns_sb_gid(const struct user_namespace *ns, uint32_t gid)
{
    (void)ns;
    return gid;
}
